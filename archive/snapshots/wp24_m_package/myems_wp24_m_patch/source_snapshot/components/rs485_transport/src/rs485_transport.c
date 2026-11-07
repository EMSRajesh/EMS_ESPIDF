#include "rs485_transport.h"
#include "ems_stack_diag.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "rs485_transport";
#define TX_DONE_WAIT_MS 1000U
#define UART_EVENT_WAIT_TICKS pdMS_TO_TICKS(10)
#define RS485_PRESERVED_APP_EVENTS RS485_DEFAULT_APP_EVENT_Q

typedef struct {
    size_t length;
    uint32_t tx_id;
    uint8_t data[RS485_MAX_TX_BYTES];
} tx_request_t;

struct rs485_transport {
    rs485_config_t config;
    QueueHandle_t uart_event_queue; /* Created by uart_driver_install(). */
    QueueHandle_t tx_queue;
    QueueHandle_t app_event_queue;
    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;
    volatile bool stopping;
    portMUX_TYPE state_lock;
    volatile rs485_recovery_state_t recovery_state;
    volatile bool recovery_request_pending;
    volatile bool recovery_release_pending;
    bool tx_dispatch_active;       /* Protected by state_lock. */
    uint32_t tx_submitters;         /* Protected by state_lock. */
    int64_t recovery_started_us;
    int64_t last_rx_activity_us;
    /*
     * True once the first bus-silence interval has been confirmed.
     * The recovery timeout bounds initial silence acquisition only. After
     * confirmation, late bytes may restart the silence interval, but must
     * not be judged against the original timeout start.
     */
    bool recovery_silence_confirmed;
#if CONFIG_MYEMS_WP24_FAULT_INJECTION
    volatile bool test_force_recovery_activity;
#endif
    rs485_event_t preserved_events[RS485_PRESERVED_APP_EVENTS];
    rs485_stats_t stats;
    /* Scratch storage owned exclusively by the transport task. */
    tx_request_t tx_scratch;
    uart_event_t uart_event_scratch;
    uint8_t rx_scratch[RS485_MAX_RX_CHUNK];
    rs485_event_t rx_event_scratch;
};

/* Single physical bus: all MyEMS-owned storage is static and bounded. */
#define RS485_TASK_STACK_SIZE_BYTES 8192U
#define RS485_TASK_STACK_WORDS (RS485_TASK_STACK_SIZE_BYTES / sizeof(StackType_t))

static StackType_t s_task_stack[RS485_TASK_STACK_WORDS];
static struct rs485_transport s_transport;
static StaticQueue_t s_tx_queue_cb, s_app_event_queue_cb;
static uint8_t s_tx_queue_storage[RS485_DEFAULT_TX_Q * sizeof(tx_request_t)] __attribute__((aligned(portBYTE_ALIGNMENT)));
static uint8_t s_app_event_queue_storage[RS485_DEFAULT_APP_EVENT_Q * sizeof(rs485_event_t)] __attribute__((aligned(portBYTE_ALIGNMENT)));
static StaticSemaphore_t s_stopped_sem_cb;
static StaticTask_t s_task_tcb;

static bool s_transport_in_use;

static void stats_increment(uint32_t *value)
{
    (void)__atomic_add_fetch(value, 1U, __ATOMIC_RELAXED);
}

static void stats_add(uint32_t *value, uint32_t amount)
{
    (void)__atomic_add_fetch(value, amount, __ATOMIC_RELAXED);
}

static void stats_update_max(uint32_t *value, uint32_t candidate)
{
    uint32_t current = __atomic_load_n(value, __ATOMIC_RELAXED);
    while (candidate > current &&
           !__atomic_compare_exchange_n(value, &current, candidate, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        /* current is refreshed by compare_exchange on failure. */
    }
}

static void post_event(struct rs485_transport *ctx, const rs485_event_t *event)
{
    if (xQueueSend(ctx->app_event_queue, event, 0) != pdTRUE) {
        stats_increment(&ctx->stats.app_event_drops);
        /* Never block UART draining on a consumer. The master also has a
           response deadline, so loss will ultimately terminate the request. */
    }
}

static void post_simple_event(struct rs485_transport *ctx,
                              rs485_event_type_t type,
                              uint32_t tx_id,
                              rs485_error_t error)
{
    rs485_event_t event = {0};
    event.type = type;
    event.tx_id = tx_id;
    event.timestamp_us = esp_timer_get_time();
    event.error = error;
    post_event(ctx, &event);
}

static rs485_recovery_state_t recovery_state_snapshot(struct rs485_transport *ctx)
{
    rs485_recovery_state_t state;
    taskENTER_CRITICAL(&ctx->state_lock);
    state = ctx->recovery_state;
    taskEXIT_CRITICAL(&ctx->state_lock);
    return state;
}

static void recovery_set_state(struct rs485_transport *ctx,
                               rs485_recovery_state_t state)
{
    taskENTER_CRITICAL(&ctx->state_lock);
    ctx->recovery_state = state;
    taskEXIT_CRITICAL(&ctx->state_lock);
}

/* Discard queued RX events while retaining TX completion/error events. */
static void discard_queued_rx_events(struct rs485_transport *ctx)
{
    size_t preserved = 0U;
    rs485_event_t event;

    while (xQueueReceive(ctx->app_event_queue, &event, 0) == pdTRUE) {
        if (event.type == RS485_EVT_TX_DONE || event.type == RS485_EVT_TX_ERROR) {
            if (preserved < RS485_PRESERVED_APP_EVENTS) {
                ctx->preserved_events[preserved++] = event;
            }
        } else {
            stats_increment(&ctx->stats.recovery_events_discarded);
        }
    }

    for (size_t i = 0U; i < preserved; ++i) {
        if (xQueueSend(ctx->app_event_queue, &ctx->preserved_events[i], 0) != pdTRUE) {
            /* No other producer exists; this indicates an invariant violation. */
            stats_increment(&ctx->stats.app_event_drops);
        }
    }
}

static void discard_queued_tx_requests(struct rs485_transport *ctx)
{
    tx_request_t discarded;
    while (xQueueReceive(ctx->tx_queue, &discarded, 0) == pdTRUE) {
        stats_increment(&ctx->stats.tx_failed);
        post_simple_event(ctx, RS485_EVT_TX_ERROR, discarded.tx_id, RS485_ERR_TX_DRIVER);
    }
}

static void begin_recovery_in_task(struct rs485_transport *ctx)
{
    taskENTER_CRITICAL(&ctx->state_lock);
    if (!ctx->recovery_request_pending) {
        taskEXIT_CRITICAL(&ctx->state_lock);
        return;
    }
    ctx->recovery_request_pending = false;
    ctx->recovery_state = RS485_RECOVERY_ACTIVE;
    taskEXIT_CRITICAL(&ctx->state_lock);

    /* This task is the sole UART/event-queue owner. */
    discard_queued_tx_requests(ctx);
    esp_err_t flush_err = uart_flush_input(ctx->config.uart_num);
    BaseType_t reset_ok = xQueueReset(ctx->uart_event_queue);
    if (flush_err != ESP_OK || reset_ok != pdPASS) {
        stats_increment(&ctx->stats.recovery_failure_count);
        recovery_set_state(ctx, RS485_RECOVERY_FAILED);
        ESP_LOGE(TAG, "RS485 recovery failed during RX cleanup: flush=%s",
                 esp_err_to_name(flush_err));
        return;
    }
    discard_queued_rx_events(ctx);

    ctx->recovery_started_us = esp_timer_get_time();
    ctx->last_rx_activity_us = ctx->recovery_started_us;
    ctx->recovery_silence_confirmed = false;
    stats_increment(&ctx->stats.recovery_count);
    ESP_LOGW(TAG, "RS485 recovery active; RX path quarantined");
}

static void recovery_drain_uart_rx(struct rs485_transport *ctx)
{
    for (;;) {
        int bytes_read = uart_read_bytes(ctx->config.uart_num,
                                         ctx->rx_scratch,
                                         RS485_MAX_RX_CHUNK,
                                         0);
        if (bytes_read <= 0) {
            break;
        }
        ctx->last_rx_activity_us = esp_timer_get_time();
        stats_add(&ctx->stats.recovery_bytes_discarded, (uint32_t)bytes_read);
        if (recovery_state_snapshot(ctx) == RS485_RECOVERY_COMPLETE) {
            recovery_set_state(ctx, RS485_RECOVERY_ACTIVE);
        }
    }
}

static void handle_recovery_uart_event(struct rs485_transport *ctx,
                                       const uart_event_t *event)
{
    switch (event->type) {
    case UART_DATA:
        ctx->last_rx_activity_us = esp_timer_get_time();
        if (recovery_state_snapshot(ctx) == RS485_RECOVERY_COMPLETE) {
            recovery_set_state(ctx, RS485_RECOVERY_ACTIVE);
        }
        recovery_drain_uart_rx(ctx);
        break;
    case UART_FIFO_OVF:
    case UART_BUFFER_FULL:
        if (recovery_state_snapshot(ctx) == RS485_RECOVERY_COMPLETE) {
            recovery_set_state(ctx, RS485_RECOVERY_ACTIVE);
        }
        stats_increment(&ctx->stats.uart_overflows);
        (void)uart_flush_input(ctx->config.uart_num);
        (void)xQueueReset(ctx->uart_event_queue);
        ctx->last_rx_activity_us = esp_timer_get_time();
        break;
    case UART_FRAME_ERR:
        if (recovery_state_snapshot(ctx) == RS485_RECOVERY_COMPLETE) {
            recovery_set_state(ctx, RS485_RECOVERY_ACTIVE);
        }
        stats_increment(&ctx->stats.uart_frame_errors);
        ctx->last_rx_activity_us = esp_timer_get_time();
        break;
    case UART_PARITY_ERR:
        if (recovery_state_snapshot(ctx) == RS485_RECOVERY_COMPLETE) {
            recovery_set_state(ctx, RS485_RECOVERY_ACTIVE);
        }
        stats_increment(&ctx->stats.uart_parity_errors);
        ctx->last_rx_activity_us = esp_timer_get_time();
        break;
    case UART_BREAK:
        if (recovery_state_snapshot(ctx) == RS485_RECOVERY_COMPLETE) {
            recovery_set_state(ctx, RS485_RECOVERY_ACTIVE);
        }
        ctx->last_rx_activity_us = esp_timer_get_time();
        break;
    default:
        break;
    }
}

static void recovery_service(struct rs485_transport *ctx)
{
    uart_event_t *event = &ctx->uart_event_scratch;
    if (xQueueReceive(ctx->uart_event_queue, event, 0) == pdTRUE) {
        handle_recovery_uart_event(ctx, event);
        while (xQueueReceive(ctx->uart_event_queue, event, 0) == pdTRUE) {
            handle_recovery_uart_event(ctx, event);
        }
    }

    /* Drain bytes even if the UART event queue overflowed or coalesced events. */
    recovery_drain_uart_rx(ctx);

    const int64_t now_us = esp_timer_get_time();
#if CONFIG_MYEMS_WP24_FAULT_INJECTION
    /* Deterministic test hook: model persistent RX activity without touching UART ISR code. */
    if (__atomic_load_n(&ctx->test_force_recovery_activity, __ATOMIC_ACQUIRE) &&
        recovery_state_snapshot(ctx) == RS485_RECOVERY_ACTIVE) {
        ctx->last_rx_activity_us = now_us;
    }
#endif
    const int64_t required_silence_us =
        (int64_t)ctx->config.recovery_bus_idle_ms * 1000LL;
    if (recovery_state_snapshot(ctx) == RS485_RECOVERY_ACTIVE &&
        !ctx->recovery_silence_confirmed &&
        (now_us - ctx->recovery_started_us) >=
            ((int64_t)ctx->config.recovery_timeout_ms * 1000LL)) {
        stats_increment(&ctx->stats.recovery_timeout_count);
        stats_increment(&ctx->stats.recovery_failure_count);
        recovery_set_state(ctx, RS485_RECOVERY_FAILED);
        ESP_LOGE(TAG, "RS485 recovery timed out after %lu ms; bus remains blocked",
                 (unsigned long)ctx->config.recovery_timeout_ms);
        return;
    }

    if (recovery_state_snapshot(ctx) == RS485_RECOVERY_ACTIVE &&
        (now_us - ctx->last_rx_activity_us) >= required_silence_us) {
        const uint32_t duration_us = (uint32_t)(now_us - ctx->recovery_started_us);
        stats_update_max(&ctx->stats.recovery_duration_max_us, duration_us);
        stats_increment(&ctx->stats.recovery_success_count);
        ctx->recovery_silence_confirmed = true;
        recovery_set_state(ctx, RS485_RECOVERY_COMPLETE);
        ESP_LOGI(TAG, "RS485 transport recovery complete; silence=%lu ms",
                 (unsigned long)ctx->config.recovery_bus_idle_ms);
    }

    /* Release is finalized only by this task after checking both UART buffers
       and the driver event queue. This prevents queued late RX from leaking
       into normal Modbus response handling. */
    if (recovery_state_snapshot(ctx) == RS485_RECOVERY_COMPLETE &&
        __atomic_load_n(&ctx->recovery_release_pending, __ATOMIC_ACQUIRE)) {
        /* Quiesce RX interrupt handling while the task performs the final
           stale-data cleanup and changes the ownership state. Any bytes still
           buffered at this boundary are discarded, never forwarded as a new
           Modbus response. */
        esp_err_t disable_err = uart_disable_rx_intr(ctx->config.uart_num);
        if (disable_err == ESP_OK) {
            (void)uart_flush_input(ctx->config.uart_num);
            (void)xQueueReset(ctx->uart_event_queue);
            size_t buffered = 0U;
            esp_err_t len_err = uart_get_buffered_data_len(ctx->config.uart_num, &buffered);
            if (len_err == ESP_OK && buffered == 0U) {
                taskENTER_CRITICAL(&ctx->state_lock);
                __atomic_store_n(&ctx->recovery_release_pending, false, __ATOMIC_RELEASE);
                ctx->recovery_state = RS485_RECOVERY_IDLE;
                taskEXIT_CRITICAL(&ctx->state_lock);
                (void)uart_enable_rx_intr(ctx->config.uart_num);
                ESP_LOGI(TAG, "RS485 recovery released; normal RX/TX resumed");
            } else {
                (void)uart_enable_rx_intr(ctx->config.uart_num);
            }
        } else {
            stats_increment(&ctx->stats.recovery_failure_count);
            recovery_set_state(ctx, RS485_RECOVERY_FAILED);
            ESP_LOGE(TAG, "Could not quiesce UART RX during recovery release: %s",
                     esp_err_to_name(disable_err));
        }
    }
}

static void drain_uart_rx(struct rs485_transport *ctx)
{
    uint8_t *buffer = ctx->rx_scratch;
    for (;;) {
        /* Zero timeout: drain only bytes currently available. */
        int bytes_read = uart_read_bytes(ctx->config.uart_num,
                                         buffer,
                                         RS485_MAX_RX_CHUNK,
                                         0);
        if (bytes_read <= 0) {
            break;
        }

        rs485_event_t *event = &ctx->rx_event_scratch;
        memset(event, 0, sizeof(*event));
        event->type = RS485_EVT_RX_DATA;
        event->timestamp_us = esp_timer_get_time();
        event->length = (size_t)bytes_read;
        memcpy(event->data, buffer, (size_t)bytes_read);
        stats_increment(&ctx->stats.rx_chunks);
        stats_add(&ctx->stats.rx_bytes, (uint32_t)bytes_read);
        post_event(ctx, event);
    }
}

static void handle_uart_event(struct rs485_transport *ctx, const uart_event_t *uart_event)
{
    switch (uart_event->type) {
    case UART_DATA:
        drain_uart_rx(ctx);
        /* UART_DATA can also be generated for receive timeout. It is only a
           timing hint, not a promise that a complete Modbus frame was received. */
        if (uart_event->timeout_flag) {
            post_simple_event(ctx, RS485_EVT_RX_IDLE, 0, RS485_ERR_NONE);
        }
        break;

    case UART_FIFO_OVF:
        stats_increment(&ctx->stats.uart_overflows);
        (void)uart_flush_input(ctx->config.uart_num);
        xQueueReset(ctx->uart_event_queue);
        post_simple_event(ctx, RS485_EVT_RX_LOSS, 0, RS485_ERR_FIFO_OVERFLOW);
        break;

    case UART_BUFFER_FULL:
        stats_increment(&ctx->stats.uart_overflows);
        (void)uart_flush_input(ctx->config.uart_num);
        xQueueReset(ctx->uart_event_queue);
        post_simple_event(ctx, RS485_EVT_RX_LOSS, 0, RS485_ERR_RING_BUFFER_FULL);
        break;

    case UART_FRAME_ERR:
        if (recovery_state_snapshot(ctx) == RS485_RECOVERY_COMPLETE) {
            recovery_set_state(ctx, RS485_RECOVERY_ACTIVE);
        }
        stats_increment(&ctx->stats.uart_frame_errors);
        post_simple_event(ctx, RS485_EVT_RX_ERROR, 0, RS485_ERR_UART_FRAME);
        break;

    case UART_PARITY_ERR:
        if (recovery_state_snapshot(ctx) == RS485_RECOVERY_COMPLETE) {
            recovery_set_state(ctx, RS485_RECOVERY_ACTIVE);
        }
        stats_increment(&ctx->stats.uart_parity_errors);
        post_simple_event(ctx, RS485_EVT_RX_ERROR, 0, RS485_ERR_UART_PARITY);
        break;

    case UART_BREAK:
        post_simple_event(ctx, RS485_EVT_RX_ERROR, 0, RS485_ERR_UART_BREAK);
        break;

    default:
        /* Other event types are not required by this transport contract. */
        break;
    }
}

static void transport_task(void *arg)
{
    struct rs485_transport *ctx = (struct rs485_transport *)arg;
    const uint32_t allocated_stack_bytes = ctx->config.task_stack_size != 0U
        ? ctx->config.task_stack_size : RS485_TASK_STACK_SIZE_BYTES;
    ems_stack_diag_t stack_diag;
    ems_stack_diag_init(&stack_diag, "rs485_transport", allocated_stack_bytes);

    while (!ctx->stopping) {
        ems_stack_diag_sample(&stack_diag, TAG);

        begin_recovery_in_task(ctx);
        rs485_recovery_state_t recovery = recovery_state_snapshot(ctx);
        if (recovery == RS485_RECOVERY_ACTIVE || recovery == RS485_RECOVERY_COMPLETE) {
            recovery_service(ctx);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (recovery == RS485_RECOVERY_REQUESTED || recovery == RS485_RECOVERY_FAILED) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        tx_request_t *tx = &ctx->tx_scratch;
        bool tx_dispatch_reserved = false;
        const bool tx_waiting = (uxQueueMessagesWaiting(ctx->tx_queue) > 0U);
        if (tx_waiting) {
            taskENTER_CRITICAL(&ctx->state_lock);
            if (ctx->recovery_state == RS485_RECOVERY_IDLE &&
                !ctx->recovery_request_pending && !ctx->tx_dispatch_active &&
                ctx->tx_submitters == 0U) {
                ctx->tx_dispatch_active = true;
                tx_dispatch_reserved = true;
            }
            taskEXIT_CRITICAL(&ctx->state_lock);
        }

        if (tx_dispatch_reserved) {
            if (xQueueReceive(ctx->tx_queue, tx, 0) == pdTRUE) {
                int written = uart_write_bytes(ctx->config.uart_num,
                                               (const char *)tx->data,
                                               tx->length);
                if (written != (int)tx->length) {
                    stats_increment(&ctx->stats.tx_failed);
                    post_simple_event(ctx, RS485_EVT_TX_ERROR, tx->tx_id, RS485_ERR_TX_DRIVER);
                } else {
                    /* This wait is bounded and only in the transport task. */
                    esp_err_t wait_err = uart_wait_tx_done(ctx->config.uart_num,
                                                           pdMS_TO_TICKS(TX_DONE_WAIT_MS));
                    if (wait_err == ESP_OK) {
                        stats_increment(&ctx->stats.tx_completed);
                        post_simple_event(ctx, RS485_EVT_TX_DONE, tx->tx_id, RS485_ERR_NONE);
                    } else {
                        stats_increment(&ctx->stats.tx_failed);
                        post_simple_event(ctx, RS485_EVT_TX_ERROR, tx->tx_id, RS485_ERR_TX_TIMEOUT);
                    }
                }
            }
            taskENTER_CRITICAL(&ctx->state_lock);
            ctx->tx_dispatch_active = false;
            taskEXIT_CRITICAL(&ctx->state_lock);
        }

        uart_event_t *event = &ctx->uart_event_scratch;
        if (xQueueReceive(ctx->uart_event_queue, event, UART_EVENT_WAIT_TICKS) == pdTRUE) {
            if (recovery_state_snapshot(ctx) == RS485_RECOVERY_IDLE) {
                handle_uart_event(ctx, event);
                while (xQueueReceive(ctx->uart_event_queue, event, 0) == pdTRUE) {
                    if (recovery_state_snapshot(ctx) == RS485_RECOVERY_IDLE) {
                        handle_uart_event(ctx, event);
                    } else {
                        handle_recovery_uart_event(ctx, event);
                    }
                }
            } else {
                handle_recovery_uart_event(ctx, event);
            }
        }
    }

    xSemaphoreGive(ctx->stopped_sem);
    for (;;) vTaskDelay(portMAX_DELAY); /* Owner suspends/deletes before reusing static stack/TCB. */
}

void rs485_config_default(rs485_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (rs485_config_t) {
        .uart_num = UART_NUM_1,
        .tx_gpio = 17,
        .rx_gpio = 18,
        .rts_gpio = 21,
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_EVEN,
        .stop_bits = UART_STOP_BITS_1,
        .rx_ring_buffer_size = 2048,
        .tx_ring_buffer_size = 512,
        .uart_event_queue_length = (int)RS485_DEFAULT_EVENT_Q,
        .task_priority = 10,
        .task_stack_size = 8192,
        .use_rts_half_duplex = true,
        .recovery_bus_idle_ms = 5U,
        .recovery_timeout_ms = 2000U
    };
}

esp_err_t rs485_open(const rs485_config_t *config, rs485_handle_t *out_handle)
{
    esp_err_t err = ESP_OK;
    bool uart_installed = false;

    if (config == NULL || out_handle == NULL) {
        ESP_LOGE(TAG, "rs485_open: NULL argument");
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;

    if (config->task_stack_size > RS485_TASK_STACK_SIZE_BYTES) {
        ESP_LOGE(TAG,
                "Requested stack %lu bytes exceeds static capacity %u bytes",
                (unsigned long)config->task_stack_size,
                (unsigned)RS485_TASK_STACK_SIZE_BYTES);
        return ESP_ERR_INVALID_ARG;
    }

    if (config->baud_rate == 0U ||
        config->tx_gpio < 0 ||
        config->rx_gpio < 0 ||
        config->rx_ring_buffer_size == 0U ||
        config->uart_event_queue_length <= 0 ||
        config->task_priority == 0U ||
        config->recovery_bus_idle_ms > 1000U ||
        config->recovery_timeout_ms > 60000U ||
        (config->use_rts_half_duplex && config->rts_gpio < 0)) {

        ESP_LOGE(TAG,
                "Invalid RS485 config: UART=%d TX=%d RX=%d RTS=%d "
                "baud=%lu RXbuf=%u TXbuf=%u eventQ=%d stack=%lu priority=%u",
                (int)config->uart_num,
                config->tx_gpio,
                config->rx_gpio,
                config->rts_gpio,
                (unsigned long)config->baud_rate,
                (unsigned int)config->rx_ring_buffer_size,
                (unsigned int)config->tx_ring_buffer_size,
                config->uart_event_queue_length,
                (unsigned long)config->task_stack_size,
                (unsigned int)config->task_priority);

        return ESP_ERR_INVALID_ARG;
    }

    /* task_stack_size is configured in bytes; xTaskCreateStatic below uses
       the statically allocated stack depth in StackType_t words. */
    if (config->task_stack_size != 0U &&
        config->task_stack_size > sizeof(s_task_stack)) {
        ESP_LOGE(TAG, "Requested stack %lu bytes exceeds static capacity %u bytes",
                 (unsigned long)config->task_stack_size,
                 (unsigned int)sizeof(s_task_stack));
        return ESP_ERR_INVALID_ARG;
    }

    if (s_transport_in_use) {
        ESP_LOGE(TAG, "RS485 transport is already open");
        return ESP_ERR_INVALID_STATE;
    }

    struct rs485_transport *ctx = &s_transport;
    memset(ctx, 0, sizeof(*ctx));
    portMUX_INITIALIZE(&ctx->state_lock);
    ctx->recovery_state = RS485_RECOVERY_IDLE;
    ctx->config = *config;
    if (ctx->config.recovery_bus_idle_ms == 0U) {
        ctx->config.recovery_bus_idle_ms = 5U;
    }
    if (ctx->config.recovery_timeout_ms == 0U) {
        ctx->config.recovery_timeout_ms = 2000U;
    }

    /* Create MyEMS-owned queues and semaphore statically. */
    ctx->tx_queue = xQueueCreateStatic(
        RS485_DEFAULT_TX_Q,
        sizeof(tx_request_t),
        s_tx_queue_storage,
        &s_tx_queue_cb);

    ctx->app_event_queue = xQueueCreateStatic(
        RS485_DEFAULT_APP_EVENT_Q,
        sizeof(rs485_event_t),
        s_app_event_queue_storage,
        &s_app_event_queue_cb);

    ctx->stopped_sem = xSemaphoreCreateBinaryStatic(&s_stopped_sem_cb);

    if (ctx->tx_queue == NULL ||
        ctx->app_event_queue == NULL ||
        ctx->stopped_sem == NULL) {
        ESP_LOGE(TAG, "Static RTOS object creation failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* ESP-IDF UART driver installation. */
    ESP_LOGI(TAG, "Installing UART%d driver", (int)config->uart_num);

    err = uart_driver_install(
        config->uart_num,
        (int)config->rx_ring_buffer_size,
        (int)config->tx_ring_buffer_size,
        config->uart_event_queue_length,
        &ctx->uart_event_queue,
        0);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned int)err);
        goto fail;
    }

    uart_installed = true;
    ESP_LOGI(TAG, "uart_driver_install: OK");

    /*
     * Configure UART framing.
     * RX flow control is disabled, so no hardware flow control is requested.
     */
    const uart_config_t uart_config = {
        .baud_rate = (int)config->baud_rate,
        .data_bits = config->data_bits,
        .parity = config->parity,
        .stop_bits = config->stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT
    };

    err = uart_param_config(config->uart_num, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned int)err);
        goto fail;
    }
    ESP_LOGI(TAG, "uart_param_config: OK");

    /*
     * Route UART signals to the configured board pins.
     * RTS is only assigned when half-duplex mode is enabled.
     */
    const int rts_pin = config->use_rts_half_duplex
                            ? config->rts_gpio
                            : UART_PIN_NO_CHANGE;

    if (config->use_rts_half_duplex && config->rts_gpio < 0) {
        ESP_LOGE(TAG, "Half-duplex enabled but RTS GPIO is invalid");
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    ESP_LOGI(TAG, "Setting pins: TX=%d RX=%d RTS=%d",
             config->tx_gpio, config->rx_gpio, rts_pin);

    err = uart_set_pin(
        config->uart_num,
        config->tx_gpio,
        config->rx_gpio,
        rts_pin,
        UART_PIN_NO_CHANGE);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned int)err);
        goto fail;
    }
    ESP_LOGI(TAG, "uart_set_pin: OK");

    if (config->use_rts_half_duplex) {
        err = uart_set_mode(config->uart_num, UART_MODE_RS485_HALF_DUPLEX);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_set_mode(RS485_HALF_DUPLEX) failed: %s (0x%x)",
                     esp_err_to_name(err), (unsigned int)err);
            goto fail;
        }
        ESP_LOGI(TAG, "uart_set_mode(RS485_HALF_DUPLEX): OK");
    }

    /*
     * RX timeout is in UART symbol periods, not milliseconds.
     * A value of 3 is retained from the existing implementation.
     */
    err = uart_set_rx_timeout(config->uart_num, 3);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_rx_timeout failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned int)err);
        goto fail;
    }
    ESP_LOGI(TAG, "uart_set_rx_timeout: OK");

    /*
     * Create the transport task with the existing static stack.
     * ESP-IDF's xTaskCreateStatic stack depth is measured in StackType_t
     * words; s_task_stack is already allocated as a StackType_t array.
     */
    const uint32_t stack_size_bytes = config->task_stack_size != 0U
        ? config->task_stack_size : RS485_TASK_STACK_SIZE_BYTES;
    const UBaseType_t stack_depth_words = (UBaseType_t)(
        (stack_size_bytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t));

    if (stack_depth_words == 0U || stack_depth_words > RS485_TASK_STACK_WORDS) {
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    ctx->task = xTaskCreateStatic(
        transport_task,
        "rs485_transport",
        stack_depth_words,
        ctx,
        config->task_priority,
        s_task_stack,
        &s_task_tcb);

    if (ctx->task == NULL) {
        ESP_LOGE(TAG, "xTaskCreateStatic failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_transport_in_use = true;
    *out_handle = ctx;

    ESP_LOGI(TAG,
             "RS485 opened successfully: UART%d TX=%d RX=%d RTS=%d baud=%lu",
             (int)config->uart_num,
             config->tx_gpio,
             config->rx_gpio,
             config->use_rts_half_duplex ? config->rts_gpio : -1,
             (unsigned long)config->baud_rate);

    return ESP_OK;

fail:
    /*
     * Unwind only resources that were successfully initialized.
     * Do not mark the transport open on a partial failure.
     */
    if (uart_installed) {
        (void)uart_driver_delete(config->uart_num);
    }

    if (ctx->tx_queue != NULL) {
        vQueueDelete(ctx->tx_queue);
        ctx->tx_queue = NULL;
    }

    if (ctx->app_event_queue != NULL) {
        vQueueDelete(ctx->app_event_queue);
        ctx->app_event_queue = NULL;
    }

    if (ctx->stopped_sem != NULL) {
        vSemaphoreDelete(ctx->stopped_sem);
        ctx->stopped_sem = NULL;
    }

    ctx->task = NULL;
    ctx->uart_event_queue = NULL;
    ESP_LOGE(TAG, "rs485_open failed at initialization: %s (0x%x)",
             esp_err_to_name(err), (unsigned int)err);

    return err;
}

esp_err_t rs485_recovery_begin(rs485_handle_t handle)
{
    if (handle == NULL || handle->stopping) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&handle->state_lock);
    if (handle->recovery_state != RS485_RECOVERY_IDLE ||
        handle->recovery_request_pending ||
        handle->tx_dispatch_active ||
        handle->tx_submitters != 0U) {
        taskEXIT_CRITICAL(&handle->state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    handle->recovery_request_pending = true;
    handle->recovery_state = RS485_RECOVERY_REQUESTED;
    stats_increment(&handle->stats.recovery_requests);
    taskEXIT_CRITICAL(&handle->state_lock);
    return ESP_OK;
}

esp_err_t rs485_recovery_get_state(rs485_handle_t handle,
                                   rs485_recovery_state_t *state)
{
    if (handle == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *state = recovery_state_snapshot(handle);
    return ESP_OK;
}

esp_err_t rs485_recovery_release(rs485_handle_t handle)
{
    if (handle == NULL || handle->stopping) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&handle->state_lock);
    if (handle->recovery_state != RS485_RECOVERY_COMPLETE ||
        __atomic_load_n(&handle->recovery_release_pending, __ATOMIC_ACQUIRE)) {
        taskEXIT_CRITICAL(&handle->state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_store_n(&handle->recovery_release_pending, true, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL(&handle->state_lock);
    return ESP_OK;
}

esp_err_t rs485_submit_tx(rs485_handle_t handle,
                          const uint8_t *data,
                          size_t length,
                          uint32_t tx_id)
{
    if (handle == NULL || data == NULL || length == 0 ||
        length > RS485_MAX_TX_BYTES || tx_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Reserve submission against recovery_begin(). The queue operation is
       outside the spinlock; the reservation prevents recovery from starting
       until this submission has either succeeded or failed. */
    taskENTER_CRITICAL(&handle->state_lock);
    if (handle->stopping || handle->recovery_state != RS485_RECOVERY_IDLE ||
        handle->recovery_request_pending) {
        taskEXIT_CRITICAL(&handle->state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    handle->tx_submitters++;
    taskEXIT_CRITICAL(&handle->state_lock);

    tx_request_t tx = {0};
    tx.length = length;
    tx.tx_id = tx_id;
    memcpy(tx.data, data, length);
    const BaseType_t queued = xQueueSend(handle->tx_queue, &tx, 0);

    taskENTER_CRITICAL(&handle->state_lock);
    handle->tx_submitters--;
    taskEXIT_CRITICAL(&handle->state_lock);

    if (queued != pdTRUE) {
        return ESP_ERR_TIMEOUT; /* Queue full: caller may retry or report overload. */
    }
    stats_increment(&handle->stats.tx_submitted);
    return ESP_OK;
}

esp_err_t rs485_get_event(rs485_handle_t handle,
                          rs485_event_t *event,
                          TickType_t timeout_ticks)
{
    if (handle == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueReceive(handle->app_event_queue, event, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t rs485_get_stats(rs485_handle_t handle, rs485_stats_t *stats)
{
    if (handle == NULL || stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Each counter is an atomic 32-bit snapshot. The set is not a single-time
       transaction, but no field is read concurrently with a non-atomic write. */
#define LOAD_STAT(field) stats->field = __atomic_load_n(&handle->stats.field, __ATOMIC_RELAXED)
    LOAD_STAT(tx_submitted);
    LOAD_STAT(tx_completed);
    LOAD_STAT(tx_failed);
    LOAD_STAT(rx_bytes);
    LOAD_STAT(rx_chunks);
    LOAD_STAT(uart_frame_errors);
    LOAD_STAT(uart_parity_errors);
    LOAD_STAT(uart_overflows);
    LOAD_STAT(app_event_drops);
    LOAD_STAT(recovery_requests);
    LOAD_STAT(recovery_count);
    LOAD_STAT(recovery_bytes_discarded);
    LOAD_STAT(recovery_events_discarded);
    LOAD_STAT(recovery_success_count);
    LOAD_STAT(recovery_failure_count);
    LOAD_STAT(recovery_timeout_count);
    LOAD_STAT(recovery_duration_max_us);
#undef LOAD_STAT
    return ESP_OK;
}

esp_err_t rs485_close(rs485_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->stopping = true;
    /* The task may be finishing a bounded uart_wait_tx_done() call. Do not
       delete driver queues or free context until the task has exited. */
    if (xSemaphoreTake(handle->stopped_sem, pdMS_TO_TICKS(TX_DONE_WAIT_MS + 500U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT; /* Safe to call close again after the task exits. */
    }
    vTaskSuspend(handle->task);
    vTaskDelete(handle->task);
    (void)uart_driver_delete(handle->config.uart_num);
    vQueueDelete(handle->tx_queue);
    vQueueDelete(handle->app_event_queue);
    vSemaphoreDelete(handle->stopped_sem);
    memset(handle, 0, sizeof(*handle));
    s_transport_in_use = false;
    return ESP_OK;
}

#if CONFIG_MYEMS_WP24_FAULT_INJECTION
esp_err_t rs485_test_inject_rx_loss(rs485_handle_t handle, rs485_error_t error)
{
    if (handle == NULL || error == RS485_ERR_NONE) return ESP_ERR_INVALID_ARG;
    if (recovery_state_snapshot(handle) != RS485_RECOVERY_IDLE) return ESP_ERR_INVALID_STATE;
    post_simple_event(handle, RS485_EVT_RX_LOSS, 0U, error);
    return ESP_OK;
}

esp_err_t rs485_test_force_recovery_activity(rs485_handle_t handle, bool enable)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    __atomic_store_n(&handle->test_force_recovery_activity, enable, __ATOMIC_RELEASE);
    return ESP_OK;
}
#endif
