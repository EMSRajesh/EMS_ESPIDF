/*
 * MyEMS acquisition task (DP-WP4 milestone D6 state).
 *
 * All decisions are in the pure-C core (ems_acq_core.c, host-tested). This
 * file adds the FreeRTOS task, the state-store lock, logging/events and the
 * periodic value dump.
 *
 * Log lines other tools parse (tools/wp4/myems_wp4_runner.py):
 *   VALUES slot=<n> <instance_id> count=<n>
 *   VAL slot=<n> <param>=<value> <unit> q=<QUALITY> age_ms=<ms|-1> seq=<n>
 *   EVT_<NAME> slot=<n> ...
 */
#include "ems_acquisition.h"
#include "ems_stack_diag.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ems_acquisition";

#define ACQ_TASK_STACK_SIZE_BYTES 6144U
#define ACQ_TASK_STACK_WORDS      (ACQ_TASK_STACK_SIZE_BYTES / sizeof(StackType_t))
#define ACQ_WARN_INTERVAL_US      (10LL * 1000000LL)   /* repeated warnings: 1 per device per 10 s */

struct ems_acquisition {
    modbus_master_handle_t master;
    ems_poll_scheduler_handle_t scheduler;
    const ems_sched_plan_t *plan;
    const ems_config_build_t *config;
    uint32_t task_stack_size;
    uint32_t completion_wait_ms;
    uint32_t dump_interval_ms;
    UBaseType_t task_priority;
    ems_acq_core_t core;                                  /* task-only            */
    ems_acquisition_stats_t stats;                        /* shared: s_stats_mux  */
    ems_acq_device_stats_t dev_stats[EMS_CFG_MAX_DEVICES];/* shared: s_stats_mux  */
    int64_t last_warn_us[EMS_CFG_MAX_DEVICES];
    int64_t next_dump_us;
    modbus_result_t result;                               /* ~520 B, off the stack */
    ems_measurement_record_t snapshot[EMS_MAX_PARAMS_PER_DEVICE];
    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;
    volatile bool stopping;
};

static struct ems_acquisition s_acq;
static StaticSemaphore_t s_stopped_sem_cb;
static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[ACQ_TASK_STACK_WORDS];
static bool s_in_use;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_store_mux = portMUX_INITIALIZER_UNLOCKED;

/* State-store lock: short critical section, held only while copying records. */
static void store_lock(void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&s_store_mux);
}

static void store_unlock(void *ctx)
{
    (void)ctx;
    taskEXIT_CRITICAL(&s_store_mux);
}

const char *ems_acquisition_status_name(modbus_result_status_t status)
{
    switch (status) {
    case MODBUS_RESULT_OK:              return "OK";
    case MODBUS_RESULT_TIMEOUT:         return "TIMEOUT";
    case MODBUS_RESULT_CRC_ERROR:       return "CRC_ERROR";
    case MODBUS_RESULT_PROTOCOL_ERROR:  return "PROTOCOL_ERROR";
    case MODBUS_RESULT_EXCEPTION:       return "EXCEPTION";
    case MODBUS_RESULT_TRANSPORT_ERROR: return "TRANSPORT_ERROR";
    case MODBUS_RESULT_CANCELLED:       return "CANCELLED";
    default:                            return "UNKNOWN";
    }
}

static const char *device_name(const struct ems_acquisition *acq, uint8_t slot)
{
    if (acq->config != NULL && slot < acq->config->device_count &&
        acq->config->devices[slot].instance_id != NULL) {
        return acq->config->devices[slot].instance_id;
    }
    return "?";
}

static const char *param_name(uint16_t id)
{
    const ems_param_info_t *p = ems_param_find(id);
    return (p != NULL) ? p->name : "?";
}

static const char *param_unit(uint16_t id)
{
    const ems_param_info_t *p = ems_param_find(id);
    return (p != NULL) ? ems_unit_symbol(p->unit) : "?";
}

static bool warn_allowed(struct ems_acquisition *acq, uint8_t slot, int64_t now_us)
{
    if (slot >= EMS_CFG_MAX_DEVICES) {
        return true;
    }
    if (acq->last_warn_us[slot] == 0 || now_us - acq->last_warn_us[slot] >= ACQ_WARN_INTERVAL_US) {
        acq->last_warn_us[slot] = now_us;
        return true;
    }
    return false;
}

static void publish_stats(struct ems_acquisition *acq, uint32_t stale_changed)
{
    ems_acquisition_stats_t st = { 0 };
    st.results = acq->core.stats.results;
    st.routing_errors = acq->core.stats.routing_errors;
    for (uint8_t s = 0; s < acq->config->device_count && s < EMS_CFG_MAX_DEVICES; s++) {
        const ems_acq_device_stats_t *d = &acq->core.dev[s];
        st.ok += d->ok;
        st.failed += d->timeouts + d->crc_errors + d->protocol_errors + d->transport_errors + d->exceptions;
        st.cancelled += d->cancelled;
    }
    taskENTER_CRITICAL(&s_stats_mux);
    st.feedback_failures = acq->stats.feedback_failures;
    st.stale_transitions = acq->stats.stale_transitions + stale_changed;
    acq->stats = st;
    memcpy(acq->dev_stats, acq->core.dev, sizeof(acq->dev_stats));
    taskEXIT_CRITICAL(&s_stats_mux);
}

static void log_outcome(struct ems_acquisition *acq, const modbus_result_t *r,
                        const ems_acq_outcome_t *o, int64_t now_us)
{
    const char *dev = device_name(acq, o->slot);
    switch ((ems_acq_outcome_kind_t)o->kind) {
    case EMS_ACQ_OUT_OK:
        ESP_LOGD(TAG, "%s grp%u request=0x%08" PRIX32 " OK: %u GOOD, %u OUT_OF_RANGE, %u INVALID",
                 dev, (unsigned)o->group_index, r->request_id, (unsigned)o->good,
                 (unsigned)o->out_of_range, (unsigned)o->invalid);
        if ((o->out_of_range != 0U || o->invalid != 0U) && warn_allowed(acq, o->slot, now_us)) {
            ESP_LOGW(TAG, "%s grp%u: %u parameter(s) OUT_OF_RANGE, %u INVALID", dev,
                     (unsigned)o->group_index, (unsigned)o->out_of_range, (unsigned)o->invalid);
        }
        if (o->counter_discontinuities != 0U) {
            ESP_LOGW(TAG, "EVT_COUNTER_DISCONTINUITY slot=%u %s param=%s: counter decreased or reset; "
                          "no delta, new baseline", (unsigned)o->slot, dev, param_name(o->discontinuity_param));
        }
        if (o->counter_rollovers != 0U) {
            ESP_LOGI(TAG, "EVT_COUNTER_ROLLOVER slot=%u %s", (unsigned)o->slot, dev);
        }
        break;
    case EMS_ACQ_OUT_MISMATCH:
        ESP_LOGW(TAG, "EVT_RESPONSE_MISMATCH slot=%u %s grp%u request=0x%08" PRIX32
                      " registers=%u: group INVALID", (unsigned)o->slot, dev, (unsigned)o->group_index,
                 r->request_id, (unsigned)r->register_count);
        break;
    case EMS_ACQ_OUT_EXCEPTION:
        ESP_LOGW(TAG, "EVT_MODBUS_EXCEPTION slot=%u %s grp%u code=0x%02X: group INVALID",
                 (unsigned)o->slot, dev, (unsigned)o->group_index, (unsigned)o->exception_code);
        break;
    case EMS_ACQ_OUT_FAILED:
        if (warn_allowed(acq, o->slot, now_us)) {
            ESP_LOGW(TAG, "%s grp%u %s (consecutive failures %u): %u parameter(s) GOOD -> STALE",
                     dev, (unsigned)o->group_index, ems_acquisition_status_name(r->status),
                     (unsigned)o->consecutive_failures, (unsigned)o->stale);
        }
        break;
    case EMS_ACQ_OUT_CANCELLED:
        ESP_LOGI(TAG, "%s grp%u request=0x%08" PRIX32 " CANCELLED", dev, (unsigned)o->group_index,
                 r->request_id);
        break;
    case EMS_ACQ_OUT_ROUTING_ERROR:
    default:
        ESP_LOGW(TAG, "routing error request=0x%08" PRIX32 " slave=%u FC%02u regs=%u status=%s: "
                      "does not match its poll entry; discarded",
                 r->request_id, (unsigned)r->slave_addr, (unsigned)r->function_code,
                 (unsigned)r->register_count, ems_acquisition_status_name(r->status));
        break;
    }
}

static void process_result(struct ems_acquisition *acq, const modbus_result_t *r)
{
    const int64_t now_us = esp_timer_get_time();
    const ems_acq_result_t in = {
        .registers = r->registers,
        .request_id = r->request_id,
        .register_count = r->register_count,
        .slave_addr = r->slave_addr,
        .function_code = r->function_code,
        .status = (uint8_t)r->status,
        .exception_code = r->exception_code,
    };
    ems_acq_outcome_t o;
    ems_acq_core_process(&acq->core, &in, now_us, &o);
    log_outcome(acq, r, &o, now_us);

    /* Every terminal result, including routing errors, releases the scheduler slot. */
    if (ems_poll_scheduler_notify_completion(acq->scheduler, r->request_id) != ESP_OK) {
        taskENTER_CRITICAL(&s_stats_mux);
        acq->stats.feedback_failures++;
        taskEXIT_CRITICAL(&s_stats_mux);
        ESP_LOGE(TAG, "scheduler feedback failed for request=0x%08" PRIX32, r->request_id);
    }
}

static void dump_values(struct ems_acquisition *acq, int64_t now_us)
{
    for (uint8_t s = 0; s < acq->config->device_count; s++) {
        size_t n = 0;
        if (ems_state_snapshot_device(s, acq->snapshot, EMS_MAX_PARAMS_PER_DEVICE, &n) != EMS_OK) {
            continue;
        }
        ESP_LOGI(TAG, "VALUES slot=%u %s count=%u", (unsigned)s, device_name(acq, s), (unsigned)n);
        for (size_t i = 0; i < n; i++) {
            const ems_measurement_record_t *rec = &acq->snapshot[i];
            const long long age_ms = rec->sample_mono_us > 0 ? (now_us - rec->sample_mono_us) / 1000 : -1;
            ESP_LOGI(TAG, "VAL slot=%u %s=%.9g %s q=%s age_ms=%lld seq=%" PRIu32,
                     (unsigned)s, param_name(rec->param_id), rec->value, param_unit(rec->param_id),
                     ems_quality_name((ems_quality_t)rec->quality), age_ms, rec->seq);
        }
    }
}

static void acquisition_task(void *argument)
{
    struct ems_acquisition *acq = argument;
    ems_stack_diag_t stack_diag;
    ems_stack_diag_init(&stack_diag, "ems_acquisition", acq->task_stack_size);
    acq->next_dump_us = esp_timer_get_time() + (int64_t)acq->dump_interval_ms * 1000;

    while (!acq->stopping) {
        ems_stack_diag_sample(&stack_diag, TAG);
        const esp_err_t err = modbus_master_get_completion(acq->master, &acq->result,
                                                           pdMS_TO_TICKS(acq->completion_wait_ms));
        if (err == ESP_OK) {
            process_result(acq, &acq->result);
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "completion read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        /* Freshness check on every wake-up, i.e. at least every completion_wait_ms. */
        const int64_t now_us = esp_timer_get_time();
        const uint32_t changed = ems_acq_core_tick(&acq->core, now_us);
        if (changed != 0U) {
            ESP_LOGW(TAG, "freshness: %" PRIu32 " parameter(s) GOOD -> STALE (age > stale timeout)", changed);
        }
        publish_stats(acq, changed);

        if (acq->dump_interval_ms != 0U && now_us >= acq->next_dump_us) {
            dump_values(acq, now_us);
            acq->next_dump_us = now_us + (int64_t)acq->dump_interval_ms * 1000;
        }
    }

    xSemaphoreGive(acq->stopped_sem);
    for (;;) {
        vTaskDelay(portMAX_DELAY);   /* owner suspends/deletes before reusing static storage */
    }
}

esp_err_t ems_acquisition_start(const ems_acquisition_config_t *config, ems_acquisition_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL || config->master == NULL || config->scheduler == NULL ||
        config->plan == NULL || config->plan->entry_count == 0U || config->config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    const uint32_t stack_bytes = config->task_stack_size != 0U ? config->task_stack_size
                                                               : ACQ_TASK_STACK_SIZE_BYTES;
    if (s_in_use || stack_bytes > ACQ_TASK_STACK_SIZE_BYTES || stack_bytes < 3072U) {
        return ESP_ERR_INVALID_ARG;
    }

    struct ems_acquisition *acq = &s_acq;
    memset(acq, 0, sizeof(*acq));
    acq->master = config->master;
    acq->scheduler = config->scheduler;
    acq->plan = config->plan;
    acq->config = config->config;
    acq->task_stack_size = stack_bytes;
    acq->task_priority = config->task_priority != 0U ? config->task_priority : 6U;
    acq->completion_wait_ms = config->completion_wait_ms != 0U ? config->completion_wait_ms : 100U;
    acq->dump_interval_ms = config->value_dump_interval_ms;

    static const ems_state_lock_t lock = { .lock = store_lock, .unlock = store_unlock, .ctx = NULL };
    const ems_acq_init_status_t ist = ems_acq_core_init(&acq->core, config->config, config->plan, &lock);
    if (ist != EMS_ACQ_INIT_OK) {
        ESP_LOGE(TAG, "acquisition core init failed: %s", ems_acq_init_status_name(ist));
        memset(acq, 0, sizeof(*acq));
        return ESP_ERR_INVALID_ARG;
    }

    acq->stopped_sem = xSemaphoreCreateBinaryStatic(&s_stopped_sem_cb);
    if (acq->stopped_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const UBaseType_t words = (UBaseType_t)((stack_bytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t));
    acq->task = xTaskCreateStatic(acquisition_task, "ems_acquisition", words, acq, acq->task_priority,
                                  s_task_stack, &s_task_tcb);
    if (acq->task == NULL) {
        vSemaphoreDelete(acq->stopped_sem);
        memset(acq, 0, sizeof(*acq));
        return ESP_ERR_NO_MEM;
    }
    s_in_use = true;
    *out_handle = acq;
    ESP_LOGI(TAG, "acquisition started: %u poll entries, state store %u device(s), value dump every %" PRIu32 " ms",
             (unsigned)acq->plan->entry_count, (unsigned)acq->config->device_count, acq->dump_interval_ms);
    return ESP_OK;
}

esp_err_t ems_acquisition_get_stats(ems_acquisition_handle_t handle, ems_acquisition_stats_t *out)
{
    if (handle == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_stats_mux);
    *out = handle->stats;
    taskEXIT_CRITICAL(&s_stats_mux);
    return ESP_OK;
}

esp_err_t ems_acquisition_get_device_stats(ems_acquisition_handle_t handle, uint8_t slot,
                                           ems_acq_device_stats_t *out)
{
    if (handle == NULL || out == NULL || slot >= EMS_CFG_MAX_DEVICES) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_stats_mux);
    *out = handle->dev_stats[slot];
    taskEXIT_CRITICAL(&s_stats_mux);
    return ESP_OK;
}

esp_err_t ems_acquisition_stop(ems_acquisition_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->stopping = true;
    if (xSemaphoreTake(handle->stopped_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    vTaskSuspend(handle->task);
    vTaskDelete(handle->task);
    vSemaphoreDelete(handle->stopped_sem);
    memset(handle, 0, sizeof(*handle));
    s_in_use = false;
    return ESP_OK;
}
