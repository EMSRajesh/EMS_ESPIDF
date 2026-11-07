#include "modbus_rtu_master.h"
#include "ems_stack_diag.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "modbus_master";
#define MASTER_LOOP_WAIT_MS 10U
#define RETRY_QUIET_TIME_MS 100U
#define MASTER_TX_DONE_TIMEOUT_MS 1250U
#define LATE_RESPONSE_GUARD_MS 1000U
#define RECOVERY_START_RETRY_MS 2000U

typedef struct { modbus_request_t request; } queued_request_t;
struct modbus_master {
    modbus_master_config_t config;
    QueueHandle_t request_queue, completion_queue;
    SemaphoreHandle_t completion_slots; /* One token reserved per accepted, unconsumed request. */
    SemaphoreHandle_t cancel_done_sem;
    SemaphoreHandle_t api_lock; /* Serializes admission against shutdown transition. */
    modbus_master_lifecycle_state_t lifecycle_state;
    volatile bool cancel_pending_requested;
    volatile bool cancel_api_busy;
    uint32_t cancelled_last_count;
    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;
    bool stopping; /* Accessed atomically; API admission is serialized by api_lock. */
    modbus_master_stats_t stats;
};

/* One master per physical RS-485 bus. MyEMS-owned storage is statically bounded. */
#define MASTER_STATIC_REQUEST_Q 16U
#define MASTER_STATIC_COMPLETION_Q 16U
#define MASTER_TASK_STACK_SIZE_BYTES 6144U
#define MASTER_TASK_STACK_WORDS (MASTER_TASK_STACK_SIZE_BYTES / sizeof(StackType_t))
static struct modbus_master s_master;

/* Master statistics are updated by both the master task and API callers.
 * Use atomic operations for every access; a plain struct copy is a data race.
 */
static void master_stat_increment(uint32_t *value)
{
    (void)__atomic_add_fetch(value, 1U, __ATOMIC_RELAXED);
}

static uint32_t master_stat_load(const uint32_t *value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}
static StaticQueue_t s_request_queue_cb, s_completion_queue_cb;
static uint8_t s_request_queue_storage[MASTER_STATIC_REQUEST_Q * sizeof(queued_request_t)] __attribute__((aligned(portBYTE_ALIGNMENT)));
static uint8_t s_completion_queue_storage[MASTER_STATIC_COMPLETION_Q * sizeof(modbus_result_t)] __attribute__((aligned(portBYTE_ALIGNMENT)));
static StaticSemaphore_t s_master_stopped_sem_cb;
static StaticSemaphore_t s_completion_slots_sem_cb;
static StaticSemaphore_t s_cancel_done_sem_cb;
static StaticSemaphore_t s_api_lock_sem_cb;
static SemaphoreHandle_t s_api_lock; /* Static-lifetime lock; never deleted between runs. */
static StaticTask_t s_master_task_tcb;
static StackType_t s_master_task_stack[MASTER_TASK_STACK_WORDS];
static bool s_master_in_use;
typedef enum {
    TXN_IDLE = 0,
    TXN_WAIT_TX_DONE,
    TXN_WAIT_RESPONSE,
    TXN_RECOVERY,
    TXN_WAIT_RECOVERY_RELEASE,
    TXN_RETRY_DELAY,
    TXN_BUS_UNAVAILABLE
} txn_state_t;
typedef struct {
    bool active;
    modbus_request_t request;
    txn_state_t state;
    uint8_t retries_used;
    int64_t deadline_us, retry_at_us;
    int64_t recovery_guard_until_us, recovery_start_deadline_us;
    modbus_result_status_t failed_status;
    bool retry_after_recovery;
    uint8_t frame[MODBUS_MAX_ADU_BYTES];
    size_t frame_length;
    uint8_t rx[MODBUS_MAX_ADU_BYTES];
    size_t rx_length, expected_length;
} transaction_t;

/* WP2.5-E1: singleton master-task workspaces, exclusively owned by master task. */
static transaction_t s_master_transaction;
static modbus_result_t s_publish_workspace;
static modbus_result_t s_decode_workspace;
static queued_request_t s_queued_workspace;
static rs485_event_t s_event_workspace;

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c=0xFFFFU;
    for(size_t i=0;i<n;i++){ c^=p[i]; for(unsigned b=0;b<8;b++) c=(c&1U)?(uint16_t)((c>>1U)^0xA001U):(uint16_t)(c>>1U); }
    return c;
}
static void add_crc(uint8_t *p,size_t n){uint16_t c=crc16(p,n);p[n]=(uint8_t)c;p[n+1]=(uint8_t)(c>>8);}
static bool crc_ok(const uint8_t *p,size_t n){if(n<4)return false;uint16_t c=crc16(p,n-2);return p[n-2]==(uint8_t)c&&p[n-1]==(uint8_t)(c>>8);}
static uint16_t be16(const uint8_t *p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)(v>>8);p[1]=(uint8_t)v;}
static bool range_ok(uint16_t start,uint16_t count){return count>0U && (uint32_t)start+(uint32_t)count<=65536UL;}
static bool master_is_stopping(const struct modbus_master *m);

static bool request_valid(const modbus_request_t *r) {
    if (!r || r->slave_addr < 1U || r->slave_addr > 247U) return false;
    if (r->max_retries != MODBUS_RETRIES_USE_DEFAULT &&
        r->max_retries > MODBUS_MAX_RETRIES) return false;
    switch(r->function_code) {
    case 1: case 2: return r->register_count<=MODBUS_MAX_COILS && range_ok(r->start_register,r->register_count);
    case 3: case 4: return r->register_count<=MODBUS_MAX_REGISTERS && range_ok(r->start_register,r->register_count);
    case 5: return r->register_count==1U && r->write_coils[0] <= 1U;
    case 6: return r->register_count==1U;
    case 15: return r->register_count<=MODBUS_MAX_WRITE_COILS && range_ok(r->start_register,r->register_count);
    case 16: return r->register_count<=MODBUS_MAX_WRITE_REGISTERS && range_ok(r->start_register,r->register_count);
    case 23: return r->register_count<=MODBUS_MAX_FC23_WRITE_REGISTERS && range_ok(r->start_register,r->register_count) &&
                    r->read_register_count<=MODBUS_MAX_REGISTERS && range_ok(r->read_start_register,r->read_register_count);
    default: return false;
    }
}
static esp_err_t build_request(transaction_t *t) {
    const modbus_request_t *r=&t->request; uint8_t *p=t->frame; size_t n=0;
    p[n++]=r->slave_addr; p[n++]=r->function_code;
    switch(r->function_code) {
    case 1: case 2: case 3: case 4:
        put16(&p[n],r->start_register);n+=2;put16(&p[n],r->register_count);n+=2;break;
    case 5:
        put16(&p[n],r->start_register);n+=2;put16(&p[n],r->write_coils[0]?0xFF00U:0U);n+=2;break;
    case 6:
        put16(&p[n],r->start_register);n+=2;put16(&p[n],r->write_registers[0]);n+=2;break;
    case 15: {
        put16(&p[n],r->start_register);n+=2;put16(&p[n],r->register_count);n+=2;
        uint8_t bc=(uint8_t)((r->register_count+7U)/8U);p[n++]=bc;
        memcpy(&p[n],r->write_coils,bc);n+=bc;break;
    }
    case 16: {
        put16(&p[n],r->start_register);n+=2;put16(&p[n],r->register_count);n+=2;
        p[n++]=(uint8_t)(r->register_count*2U);
        for(uint16_t i=0;i<r->register_count;i++){put16(&p[n],r->write_registers[i]);n+=2;}break;
    }
    case 23: {
        put16(&p[n],r->read_start_register);n+=2;put16(&p[n],r->read_register_count);n+=2;
        put16(&p[n],r->start_register);n+=2;put16(&p[n],r->register_count);n+=2;
        p[n++]=(uint8_t)(r->register_count*2U);
        for(uint16_t i=0;i<r->register_count;i++){put16(&p[n],r->write_registers[i]);n+=2;}break;
    }
    default:return ESP_ERR_NOT_SUPPORTED;
    }
    add_crc(p,n);t->frame_length=n+2U;return ESP_OK;
}
static void publish(struct modbus_master *m,const transaction_t *t,modbus_result_status_t status,uint8_t ex,
                    const modbus_result_t *payload) {
    modbus_result_t *r = &s_publish_workspace;
    memset(r, 0, sizeof(*r));
    r->request_id=t->request.request_id;r->slave_addr=t->request.slave_addr;
    r->function_code=t->request.function_code;r->status=status;r->exception_code=ex;r->retry_count=t->retries_used;
    if(payload && status==MODBUS_RESULT_OK){
        r->register_count=payload->register_count;
        memcpy(r->registers,payload->registers,sizeof(r->registers));
        r->bit_count=payload->bit_count;
        memcpy(r->bits,payload->bits,sizeof(r->bits));
    }
    /* Every accepted request owns one completion-slot token until its result is
       consumed. With one producer and no other completion writers, this send
       cannot fail unless the reservation invariant or queue ownership is broken. */
    if (xQueueSend(m->completion_queue, r, 0) == pdTRUE) {
        master_stat_increment(&m->stats.requests_completed);
    } else {
        master_stat_increment(&m->stats.completion_drops);
        ESP_LOGE(TAG, "FATAL: reserved completion slot unavailable; request=%lu",
                 (unsigned long)r->request_id);
    }
}
static void reset_rx(transaction_t *t){t->rx_length=0;t->expected_length=0;}

static bool request_is_write(uint8_t function_code)
{
    return function_code == 5U || function_code == 6U ||
           function_code == 15U || function_code == 16U ||
           function_code == 23U;
}

static uint8_t request_retry_limit(const struct modbus_master *m,
                                   const modbus_request_t *request)
{
    if (request->max_retries == MODBUS_RETRIES_USE_DEFAULT) {
        return m->config.default_retries;
    }
    return request->max_retries;
}

static void record_final_error(struct modbus_master *m, modbus_result_status_t status)
{
    if (status == MODBUS_RESULT_TIMEOUT) master_stat_increment(&m->stats.timeouts);
    if (status == MODBUS_RESULT_CRC_ERROR) master_stat_increment(&m->stats.crc_errors);
    if (status == MODBUS_RESULT_PROTOCOL_ERROR) master_stat_increment(&m->stats.protocol_errors);
    if (status == MODBUS_RESULT_TRANSPORT_ERROR) master_stat_increment(&m->stats.transport_errors);
}

/* Every failed on-wire attempt is quarantined before retry OR next request. */
static void finish_or_retry(struct modbus_master *m, transaction_t *t,
                            modbus_result_status_t status)
{
    uint8_t max = request_retry_limit(m, &t->request);
    if (request_is_write(t->request.function_code) &&
        !t->request.allow_write_retries) max = 0U;

    t->failed_status = status;
    t->retry_after_recovery = (t->retries_used < max) && !master_is_stopping(m);
    if (t->retry_after_recovery) t->retries_used++;
    t->recovery_guard_until_us = esp_timer_get_time() +
        (int64_t)LATE_RESPONSE_GUARD_MS * 1000LL;
    t->recovery_start_deadline_us = esp_timer_get_time() +
        (int64_t)RECOVERY_START_RETRY_MS * 1000LL;
    t->state = TXN_RECOVERY;
    reset_rx(t);
    ESP_LOGW(TAG, "request %lu failed status=%d; entering RS485 recovery (retry=%u/%u)",
             (unsigned long)t->request.request_id, (int)status,
             t->retries_used, max);
}

static void recovery_fail_closed(struct modbus_master *m, transaction_t *t,
                                 const char *reason)
{
    ESP_LOGE(TAG, "RS485 bus unavailable; recovery failed: %s", reason);
    record_final_error(m, MODBUS_RESULT_TRANSPORT_ERROR);
    publish(m, t, MODBUS_RESULT_TRANSPORT_ERROR, 0, NULL);
    memset(t, 0, sizeof(*t));
    t->state = TXN_BUS_UNAVAILABLE;
}

static void service_recovery(struct modbus_master *m, transaction_t *t, int64_t now_us)
{
    rs485_recovery_state_t state;
    esp_err_t err;

    if (t->state == TXN_RECOVERY) {
        err = rs485_recovery_get_state(m->config.transport, &state);
        if (err != ESP_OK) {
            recovery_fail_closed(m, t, "get_state API error");
            return;
        }
        if (state == RS485_RECOVERY_IDLE) {
            err = rs485_recovery_begin(m->config.transport);
            if (err != ESP_OK) {
                if (now_us >= t->recovery_start_deadline_us)
                    recovery_fail_closed(m, t, "could not start recovery before deadline");
                return;
            }
            ESP_LOGI(TAG, "RS485 recovery requested; late-response guard=%u ms",
                     (unsigned)LATE_RESPONSE_GUARD_MS);
            return;
        }
        if (state == RS485_RECOVERY_FAILED) {
            recovery_fail_closed(m, t, "transport reported FAILED");
            return;
        }
        if (state != RS485_RECOVERY_COMPLETE &&
            now_us >= t->recovery_start_deadline_us) {
            recovery_fail_closed(m, t, "recovery operation deadline expired");
            return;
        }
        if (state == RS485_RECOVERY_COMPLETE && now_us >= t->recovery_guard_until_us) {
            err = rs485_recovery_release(m->config.transport);
            if (err != ESP_OK) {
                recovery_fail_closed(m, t, "release request failed");
                return;
            }
            t->state = TXN_WAIT_RECOVERY_RELEASE;
        }
        return;
    }

    if (t->state == TXN_WAIT_RECOVERY_RELEASE) {
        err = rs485_recovery_get_state(m->config.transport, &state);
        if (err != ESP_OK) {
            recovery_fail_closed(m, t, "get_state while releasing failed");
            return;
        }
        if (state == RS485_RECOVERY_FAILED) {
            recovery_fail_closed(m, t, "transport failed during release");
            return;
        }
        if (state != RS485_RECOVERY_IDLE) return;

        if (t->retry_after_recovery && !master_is_stopping(m)) {
            t->retry_at_us = now_us + (int64_t)RETRY_QUIET_TIME_MS * 1000LL;
            t->state = TXN_RETRY_DELAY;
            ESP_LOGI(TAG, "recovery complete; retry scheduled after %u ms",
                     (unsigned)RETRY_QUIET_TIME_MS);
        } else {
            record_final_error(m, t->failed_status);
            publish(m, t, t->failed_status, 0, NULL);
            memset(t, 0, sizeof(*t));
        }
    }
}
static void begin_attempt(struct modbus_master *m,transaction_t *t) {
    if(build_request(t)!=ESP_OK){publish(m,t,MODBUS_RESULT_PROTOCOL_ERROR,0,NULL);memset(t,0,sizeof(*t));return;}
    reset_rx(t);esp_err_t e=rs485_submit_tx(m->config.transport,t->frame,t->frame_length,t->request.request_id);
    if(e!=ESP_OK){finish_or_retry(m,t,MODBUS_RESULT_TRANSPORT_ERROR);return;}
    t->state=TXN_WAIT_TX_DONE;t->deadline_us=esp_timer_get_time()+(int64_t)MASTER_TX_DONE_TIMEOUT_MS*1000LL;
}
static void decode(struct modbus_master *m,transaction_t *t,const uint8_t *f,size_t n) {
    const modbus_request_t *q=&t->request;
    modbus_result_t *out=&s_decode_workspace;
    memset(out, 0, sizeof(*out));
    if(!crc_ok(f,n)){finish_or_retry(m,t,MODBUS_RESULT_CRC_ERROR);return;}
    if(f[0]!=q->slave_addr){
        master_stat_increment(&m->stats.unexpected_slave_frames);
        ESP_LOGW(TAG, "unexpected slave response: expected=%u received=%u",
                 q->slave_addr, f[0]);
        reset_rx(t);
        return;
    }
    if(f[1]==(uint8_t)(q->function_code|0x80U)){publish(m,t,MODBUS_RESULT_EXCEPTION,f[2],NULL);memset(t,0,sizeof(*t));return;}
    if(f[1]!=q->function_code){finish_or_retry(m,t,MODBUS_RESULT_PROTOCOL_ERROR);return;}
    if(q->function_code==1U||q->function_code==2U||q->function_code==3U||q->function_code==4U||q->function_code==23U) {
        uint16_t count=(q->function_code==1U||q->function_code==2U)?q->register_count:
                       (q->function_code==23U?q->read_register_count:q->register_count);
        uint8_t expected_bc=(q->function_code==1U||q->function_code==2U)?(uint8_t)((count+7U)/8U):(uint8_t)(count*2U);
        if(n!=(size_t)(5U+f[2])||f[2]!=expected_bc){finish_or_retry(m,t,MODBUS_RESULT_PROTOCOL_ERROR);return;}
        if(q->function_code==1U||q->function_code==2U){out->bit_count=count;memcpy(out->bits,&f[3],expected_bc);}
        else {out->register_count=count;for(uint16_t i=0;i<count;i++)out->registers[i]=be16(&f[3+2U*i]);}
    } else if(q->function_code==5U||q->function_code==6U||q->function_code==15U||q->function_code==16U) {
        if(n!=8U||memcmp(f,&t->frame[0],6U)!=0){finish_or_retry(m,t,MODBUS_RESULT_PROTOCOL_ERROR);return;}
    } else {finish_or_retry(m,t,MODBUS_RESULT_PROTOCOL_ERROR);return;}
    publish(m,t,MODBUS_RESULT_OK,0,out);memset(t,0,sizeof(*t));
}
static void rx_byte(struct modbus_master *m,transaction_t *t,uint8_t b) {
    if(!t->active||t->state!=TXN_WAIT_RESPONSE)return;
    if(t->rx_length>=sizeof(t->rx)){finish_or_retry(m,t,MODBUS_RESULT_PROTOCOL_ERROR);return;}
    t->rx[t->rx_length++]=b;
    if(t->rx_length==2U&&t->rx[1]!=(uint8_t)t->request.function_code&&t->rx[1]!=(uint8_t)(t->request.function_code|0x80U)){reset_rx(t);return;}
    if(t->rx_length==3U) {
        if(t->rx[1]==(uint8_t)(t->request.function_code|0x80U))t->expected_length=5U;
        else if(t->request.function_code==1U||t->request.function_code==2U||t->request.function_code==3U||t->request.function_code==4U||t->request.function_code==23U)t->expected_length=5U+t->rx[2];
        else t->expected_length=8U;
        if(t->expected_length>sizeof(t->rx)){finish_or_retry(m,t,MODBUS_RESULT_PROTOCOL_ERROR);return;}
    }
    if(t->expected_length&&t->rx_length==t->expected_length){size_t n=t->rx_length;decode(m,t,t->rx,n);}
}
static void transport_event(struct modbus_master *m,transaction_t *t,const rs485_event_t *e) {
    if(e->type==RS485_EVT_TX_DONE&&t->active&&t->state==TXN_WAIT_TX_DONE&&e->tx_id==t->request.request_id){t->state=TXN_WAIT_RESPONSE;uint32_t to=t->request.timeout_ms?t->request.timeout_ms:m->config.default_timeout_ms;t->deadline_us=esp_timer_get_time()+(int64_t)to*1000LL;reset_rx(t);return;}
    if(e->type==RS485_EVT_TX_ERROR&&t->active&&e->tx_id==t->request.request_id){finish_or_retry(m,t,MODBUS_RESULT_TRANSPORT_ERROR);return;}
    if (e->type == RS485_EVT_RX_LOSS || e->type == RS485_EVT_RX_ERROR) {
        if (t->active && t->state == TXN_WAIT_RESPONSE) {
            ESP_LOGW(TAG, "receive-side transport fault=%d; terminating current attempt",
                     (int)e->error);
            finish_or_retry(m, t, MODBUS_RESULT_TRANSPORT_ERROR);
        }
        return;
    }
    if (e->type == RS485_EVT_RX_DATA && t->active &&
        t->state == TXN_WAIT_RESPONSE) {
        if (e->length > RS485_MAX_RX_CHUNK) {
            finish_or_retry(m, t, MODBUS_RESULT_PROTOCOL_ERROR);
            return;
        }
        for (size_t i = 0; i < e->length; i++) {
            rx_byte(m, t, e->data[i]);
            if (!t->active || t->state != TXN_WAIT_RESPONSE) break;
        }
    }
}

/*
 * Cancellation is serialized by the master task so only that task removes
 * requests from request_queue or terminates the active transaction.
 *
 * Caller contract for this API: stop submitting new requests before calling
 * cancel_pending. WP2.5-B will close API admission and make this atomic with
 * respect to concurrent submit/stop calls.
 */
static void publish_queued_cancelled(struct modbus_master *m,
                                      const modbus_request_t *request)
{
    modbus_result_t *r = &s_publish_workspace;
    memset(r, 0, sizeof(*r));
    r->request_id = request->request_id;
    r->slave_addr = request->slave_addr;
    r->function_code = request->function_code;
    r->status = MODBUS_RESULT_CANCELLED;
    if (xQueueSend(m->completion_queue, r, 0) == pdTRUE) {
        master_stat_increment(&m->stats.requests_completed);
    } else {
        master_stat_increment(&m->stats.completion_drops);
        ESP_LOGE(TAG, "FATAL: reserved completion slot unavailable; cancelled request=%lu",
                 (unsigned long)r->request_id);
    }
}

static void service_cancel_pending(struct modbus_master *m, transaction_t *active)
{
    if (!__atomic_exchange_n(&m->cancel_pending_requested, false, __ATOMIC_ACQ_REL)) {
        return;
    }

    uint32_t cancelled = 0U;

    if (active->active) {
        publish(m, active, MODBUS_RESULT_CANCELLED, 0U, NULL);
        master_stat_increment(&m->stats.requests_cancelled);
        master_stat_increment(&m->stats.active_requests_cancelled);
        memset(active, 0, sizeof(*active));
        cancelled++;
    }

    while (xQueueReceive(m->request_queue, &s_queued_workspace, 0) == pdTRUE) {
        publish_queued_cancelled(m, &s_queued_workspace.request);
        master_stat_increment(&m->stats.requests_cancelled);
        master_stat_increment(&m->stats.queued_requests_cancelled);
        cancelled++;
    }

    __atomic_store_n(&m->cancelled_last_count, cancelled, __ATOMIC_RELEASE);
    if (xSemaphoreGive(m->cancel_done_sem) != pdTRUE) {
        ESP_LOGE(TAG, "cancel acknowledgement semaphore invariant failure");
    }
}

static bool master_is_stopping(const struct modbus_master *m)
{
    return __atomic_load_n(&m->stopping, __ATOMIC_ACQUIRE);
}

static void master_set_stopping(struct modbus_master *m, bool stopping)
{
    __atomic_store_n(&m->stopping, stopping, __ATOMIC_RELEASE);
}

static esp_err_t master_api_lock(struct modbus_master *m, TickType_t wait)
{
    if (!m || !m->api_lock) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(m->api_lock, wait) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void master_api_unlock(struct modbus_master *m)
{
    (void)xSemaphoreGive(m->api_lock);
}

static void master_task(void *arg) {
    struct modbus_master *m=arg;
    transaction_t *t = &s_master_transaction;
    memset(t, 0, sizeof(*t));
    const uint32_t allocated_stack_bytes = m->config.task_stack_size != 0U
        ? m->config.task_stack_size : MASTER_TASK_STACK_SIZE_BYTES;
    ems_stack_diag_t stack_diag;
    ems_stack_diag_init(&stack_diag, "modbus_master", allocated_stack_bytes);
    for (;;) {
        /* Shutdown has priority over all normal transaction processing. */
        if (master_is_stopping(m)) {
            break;
        }

        ems_stack_diag_sample(&stack_diag, TAG);
        service_cancel_pending(m, t);

        /* Runtime cancellation and stop are serialized by api_lock. */
        if (master_is_stopping(m)) {
            break;
        }

        if (!t->active && t->state != TXN_BUS_UNAVAILABLE) {
            if (xQueueReceive(m->request_queue, &s_queued_workspace, 0) == pdTRUE) {
                t->active = true;
                t->request = s_queued_workspace.request;
                t->state = TXN_IDLE;
                t->retries_used = 0U;

                /* A request dequeued concurrently with stop is cancelled below,
                 * not transmitted after admission has closed. */
                if (master_is_stopping(m)) {
                    break;
                }
                begin_attempt(m, t);
            }
        }

        if (master_is_stopping(m)) {
            break;
        }

        if (rs485_get_event(m->config.transport, &s_event_workspace,
                            pdMS_TO_TICKS(MASTER_LOOP_WAIT_MS)) == ESP_OK) {
            /* Do not decode a response or publish success after shutdown wins. */
            if (master_is_stopping(m)) {
                break;
            }
            transport_event(m, t, &s_event_workspace);

            while (!master_is_stopping(m) &&
                   rs485_get_event(m->config.transport, &s_event_workspace, 0) == ESP_OK) {
                if (master_is_stopping(m)) {
                    break;
                }
                transport_event(m, t, &s_event_workspace);
            }
        }

        if (master_is_stopping(m)) {
            break;
        }

        service_cancel_pending(m, t);
        if (master_is_stopping(m)) {
            break;
        }

        int64_t now = esp_timer_get_time();
        if (t->active && (t->state == TXN_RECOVERY ||
                          t->state == TXN_WAIT_RECOVERY_RELEASE)) {
            service_recovery(m, t, now);
        }

        /* A stop arriving during recovery must suppress retry scheduling. */
        if (master_is_stopping(m)) {
            break;
        }

        if (t->active && t->state == TXN_WAIT_TX_DONE && now >= t->deadline_us) {
            finish_or_retry(m, t, MODBUS_RESULT_TRANSPORT_ERROR);
        } else if (t->active && t->state == TXN_WAIT_RESPONSE && now >= t->deadline_us) {
            finish_or_retry(m, t, MODBUS_RESULT_TIMEOUT);
        } else if (t->active && t->state == TXN_RETRY_DELAY && now >= t->retry_at_us) {
            if (!master_is_stopping(m)) {
                begin_attempt(m, t);
            }
        }

        ems_stack_diag_sample(&stack_diag, TAG);
    }
    /* Stop also terminates the active request and drains every accepted
       queued request before the owner tears down the queues. */
    if(t->active) {
        publish(m,t,MODBUS_RESULT_CANCELLED,0,NULL);
        master_stat_increment(&m->stats.requests_cancelled);
        master_stat_increment(&m->stats.active_requests_cancelled);
    }
    while (xQueueReceive(m->request_queue, &s_queued_workspace, 0) == pdTRUE) {
        publish_queued_cancelled(m, &s_queued_workspace.request);
        master_stat_increment(&m->stats.requests_cancelled);
        master_stat_increment(&m->stats.queued_requests_cancelled);
    }
    xSemaphoreGive(m->stopped_sem);for (;;) vTaskDelay(portMAX_DELAY); /* Owner suspends/deletes static task. */
}
void modbus_master_config_default(modbus_master_config_t *c,rs485_handle_t transport){if(!c)return;*c=(modbus_master_config_t){.transport=transport,.task_priority=9,.task_stack_size=6144,.default_timeout_ms=500,.default_retries=1,.request_queue_length=16,.completion_queue_length=16};}
esp_err_t modbus_master_start(const modbus_master_config_t *c,modbus_master_handle_t *out){
    if (!c || !out || !c->transport || !c->request_queue_length || !c->completion_queue_length ||
        c->request_queue_length > MASTER_STATIC_REQUEST_Q ||
        c->completion_queue_length > MASTER_STATIC_COMPLETION_Q ||
        c->task_stack_size > MASTER_TASK_STACK_SIZE_BYTES ||
        c->task_priority == 0U || c->default_timeout_ms == 0U ||
        c->default_retries > MODBUS_MAX_RETRIES) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    /* The static mutex is retained for the lifetime of the process so a stale
       handle cannot race mutex destruction during a later stop/deinit phase. */
    if (s_api_lock == NULL) {
        s_api_lock = xSemaphoreCreateMutexStatic(&s_api_lock_sem_cb);
        if (s_api_lock == NULL) return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_api_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    if (s_master_in_use) {
        (void)xSemaphoreGive(s_api_lock);
        return ESP_ERR_INVALID_STATE;
    }

    struct modbus_master *m = &s_master;
    memset(m, 0, sizeof(*m));
    m->api_lock = s_api_lock;
    m->lifecycle_state = MODBUS_MASTER_STARTING;
    memset(&s_master_transaction, 0, sizeof(s_master_transaction));
    memset(&s_publish_workspace, 0, sizeof(s_publish_workspace));
    memset(&s_decode_workspace, 0, sizeof(s_decode_workspace));
    memset(&s_queued_workspace, 0, sizeof(s_queued_workspace));
    memset(&s_event_workspace, 0, sizeof(s_event_workspace));
    m->config = *c;
    m->request_queue = xQueueCreateStatic(c->request_queue_length, sizeof(queued_request_t),
                                          s_request_queue_storage, &s_request_queue_cb);
    m->completion_queue = xQueueCreateStatic(c->completion_queue_length, sizeof(modbus_result_t),
                                             s_completion_queue_storage, &s_completion_queue_cb);
    m->stopped_sem = xSemaphoreCreateBinaryStatic(&s_master_stopped_sem_cb);
    m->completion_slots = xSemaphoreCreateCountingStatic(
        c->completion_queue_length, c->completion_queue_length,
        &s_completion_slots_sem_cb);
    m->cancel_done_sem = xSemaphoreCreateBinaryStatic(&s_cancel_done_sem_cb);
    if (!m->request_queue || !m->completion_queue || !m->stopped_sem ||
        !m->completion_slots || !m->cancel_done_sem) {
        if (m->request_queue) vQueueDelete(m->request_queue);
        if (m->completion_queue) vQueueDelete(m->completion_queue);
        if (m->stopped_sem) vSemaphoreDelete(m->stopped_sem);
        if (m->completion_slots) vSemaphoreDelete(m->completion_slots);
        if (m->cancel_done_sem) vSemaphoreDelete(m->cancel_done_sem);
        memset(m, 0, sizeof(*m));
        (void)xSemaphoreGive(s_api_lock);
        return ESP_ERR_NO_MEM;
    }
    uint32_t stack_size_bytes = c->task_stack_size != 0U
        ? c->task_stack_size
        : MASTER_TASK_STACK_SIZE_BYTES;
    UBaseType_t stack_depth_words = (UBaseType_t)(
        (stack_size_bytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t));
    if (stack_depth_words == 0U || stack_depth_words > MASTER_TASK_STACK_WORDS) {
        vQueueDelete(m->request_queue);
        vQueueDelete(m->completion_queue);
        vSemaphoreDelete(m->stopped_sem);
        vSemaphoreDelete(m->completion_slots);
        vSemaphoreDelete(m->cancel_done_sem);
        memset(m, 0, sizeof(*m));
        (void)xSemaphoreGive(s_api_lock);
        return ESP_ERR_INVALID_ARG;
    }
    m->task = xTaskCreateStatic(master_task, "modbus_master", stack_depth_words,
                                m, c->task_priority, s_master_task_stack, &s_master_task_tcb);
    if (m->task == NULL) {
        vQueueDelete(m->request_queue);
        vQueueDelete(m->completion_queue);
        vSemaphoreDelete(m->stopped_sem);
        vSemaphoreDelete(m->completion_slots);
        vSemaphoreDelete(m->cancel_done_sem);
        memset(m, 0, sizeof(*m));
        (void)xSemaphoreGive(s_api_lock);
        return ESP_ERR_NO_MEM;
    }
    m->api_lock = s_api_lock;
    m->lifecycle_state = MODBUS_MASTER_RUNNING;
    s_master_in_use = true;
    *out = m;
    (void)xSemaphoreGive(s_api_lock);
    return ESP_OK;
}

esp_err_t modbus_master_submit(modbus_master_handle_t h, const modbus_request_t *r)
{
    if (!h || !request_valid(r)) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = master_api_lock(h, 0);
    if (lock_err != ESP_OK) return lock_err;
    if (h->lifecycle_state != MODBUS_MASTER_RUNNING) {
        master_api_unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    /* Admission lock covers state check, completion reservation and enqueue.
       Therefore shutdown cannot transition to QUIESCING between acceptance
       validation and queue insertion. */
    if (xSemaphoreTake(h->completion_slots, 0) != pdTRUE) {
        master_stat_increment(&h->stats.completion_backpressure_rejections);
        master_api_unlock(h);
        return ESP_ERR_TIMEOUT;
    }

    queued_request_t q = {.request = *r};
    if (xQueueSend(h->request_queue, &q, 0) != pdTRUE) {
        (void)xSemaphoreGive(h->completion_slots);
        master_api_unlock(h);
        return ESP_ERR_TIMEOUT;
    }

    master_stat_increment(&h->stats.requests_accepted);
    master_api_unlock(h);
    return ESP_OK;
}


esp_err_t modbus_master_cancel_pending(modbus_master_handle_t h,
                                       TickType_t timeout_ticks,
                                       uint32_t *cancelled_count)
{
    if (cancelled_count) {
        *cancelled_count = 0U;
    }
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t lock_err = master_api_lock(h, 0);
    if (lock_err != ESP_OK) return lock_err;
    if (h->lifecycle_state != MODBUS_MASTER_RUNNING) {
        master_api_unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    bool expected = false;
    if (!__atomic_compare_exchange_n(&h->cancel_api_busy, &expected, true,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        master_api_unlock(h);
        return ESP_ERR_INVALID_STATE;
    }
    /* Keep the admission lock until the cancellation acknowledgement arrives.
       Shutdown therefore cannot overlap this runtime cancellation operation. */

    /* Remove an acknowledgement left by a caller that previously timed out. */
    (void)xSemaphoreTake(h->cancel_done_sem, 0);
    __atomic_store_n(&h->cancel_pending_requested, true, __ATOMIC_RELEASE);

    if (xSemaphoreTake(h->cancel_done_sem, timeout_ticks) != pdTRUE) {
        __atomic_store_n(&h->cancel_api_busy, false, __ATOMIC_RELEASE);
        master_api_unlock(h);
        return ESP_ERR_TIMEOUT;
    }

    uint32_t count = __atomic_load_n(&h->cancelled_last_count, __ATOMIC_ACQUIRE);
    if (cancelled_count) {
        *cancelled_count = count;
    }
    __atomic_store_n(&h->cancel_api_busy, false, __ATOMIC_RELEASE);
    master_api_unlock(h);
    return ESP_OK;
}

esp_err_t modbus_master_get_completion(modbus_master_handle_t h,
                                       modbus_result_t *r, TickType_t t)
{
    if (!h || !r) return ESP_ERR_INVALID_ARG;
    if (xQueueReceive(h->completion_queue, r, t) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* Release only after the result has been copied to the caller. */
    if (xSemaphoreGive(h->completion_slots) != pdTRUE) {
        ESP_LOGE(TAG, "completion reservation accounting failure");
        return ESP_FAIL;
    }
    return ESP_OK;
}
esp_err_t modbus_master_get_stats(modbus_master_handle_t h,modbus_master_stats_t *s)
{
    if (!h || !s) return ESP_ERR_INVALID_ARG;

    /* Independent atomic snapshots: counters are race-free, but the group
       is not guaranteed to represent one exact instant in time. */
#define LOAD_MASTER_STAT(field) s->field = master_stat_load(&h->stats.field)
    LOAD_MASTER_STAT(requests_accepted);
    LOAD_MASTER_STAT(requests_completed);
    LOAD_MASTER_STAT(timeouts);
    LOAD_MASTER_STAT(crc_errors);
    LOAD_MASTER_STAT(protocol_errors);
    LOAD_MASTER_STAT(transport_errors);
    LOAD_MASTER_STAT(completion_drops);
    LOAD_MASTER_STAT(completion_backpressure_rejections);
    LOAD_MASTER_STAT(requests_cancelled);
    LOAD_MASTER_STAT(active_requests_cancelled);
    LOAD_MASTER_STAT(queued_requests_cancelled);
    LOAD_MASTER_STAT(unexpected_slave_frames);
#undef LOAD_MASTER_STAT
    return ESP_OK;
}
esp_err_t modbus_master_stop(modbus_master_handle_t h)
{
    if (!h) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = master_api_lock(h, portMAX_DELAY);
    if (lock_err != ESP_OK) return lock_err;

    if (h->lifecycle_state == MODBUS_MASTER_DRAINING) {
        master_api_unlock(h);
        return ESP_OK;
    }
    if (h->lifecycle_state != MODBUS_MASTER_RUNNING &&
        h->lifecycle_state != MODBUS_MASTER_QUIESCING &&
        h->lifecycle_state != MODBUS_MASTER_FAILED) {
        master_api_unlock(h);
        return ESP_ERR_INVALID_STATE;
    }

    /* Keep api_lock for the entire stop operation. This serializes concurrent
     * stop callers and prevents a second caller from taking the same stopped
     * acknowledgement or deleting the same resources. The master task never
     * takes api_lock, so it can finish its bounded shutdown path independently.
     */
    h->lifecycle_state = MODBUS_MASTER_QUIESCING;
    master_set_stopping(h, true);

    if (xSemaphoreTake(h->stopped_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        /* Keep all resources intact: the task may still be using them. A later
         * stop call may retry waiting for the same task-exit acknowledgement.
         */
        h->lifecycle_state = MODBUS_MASTER_FAILED;
        master_api_unlock(h);
        return ESP_ERR_TIMEOUT;
    }

    /* Phase 1 (Option A): stop the producer, but preserve the completion queue
     * and its reservation semaphore for the application to drain. Deinit and
     * resource destruction are deliberately deferred to WP2.5-B3/B4.
     */
    if (h->task) {
        vTaskDelete(h->task);
        h->task = NULL;
    }
    if (h->request_queue) {
        vQueueDelete(h->request_queue);
        h->request_queue = NULL;
    }
    if (h->stopped_sem) {
        vSemaphoreDelete(h->stopped_sem);
        h->stopped_sem = NULL;
    }
    if (h->cancel_done_sem) {
        vSemaphoreDelete(h->cancel_done_sem);
        h->cancel_done_sem = NULL;
    }

    h->lifecycle_state = MODBUS_MASTER_DRAINING;
    master_api_unlock(h);
    return ESP_OK;
}

esp_err_t modbus_master_get_lifecycle_state(modbus_master_handle_t h,
                                            modbus_master_lifecycle_state_t *state)
{
    if (!h || !state) return ESP_ERR_INVALID_ARG;
    esp_err_t lock_err = master_api_lock(h, portMAX_DELAY);
    if (lock_err != ESP_OK) return lock_err;
    *state = h->lifecycle_state;
    master_api_unlock(h);
    return ESP_OK;
}

