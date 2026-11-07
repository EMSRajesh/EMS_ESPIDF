/*
 * MyEMS poll scheduler task (DP-WP4 4E, milestone D5).
 *
 * All decisions are made by the pure-C core (ems_sched_core.c, host-tested).
 * This file only adds the FreeRTOS task, the message queue, the Modbus
 * submission and logging. Changes from 09_057:
 *   - poll entries (device, poll group) from the generated configuration
 *     instead of fixed meter profiles; up to 16 entries;
 *   - request ID = (entry << 24) | seq24, so acquisition routes by ID;
 *   - one request in flight per device, earliest deadline first;
 *   - probe mode for OFFLINE devices;
 *   - guard expiry NO LONGER releases the slot (Design 9.5): it is counted,
 *     logged once and raises HEALTH_SCHED_FEEDBACK; only matching feedback
 *     releases the slot.
 */
#include "ems_poll_scheduler.h"
#include "ems_stack_diag.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ems_scheduler";

#define SCHEDULER_MAX_WAIT_MS             50U
#define SCHEDULER_STATIC_QUEUE_LEN        16U
#define SCHEDULER_TASK_STACK_SIZE_BYTES   4096U
#define SCHEDULER_TASK_STACK_WORDS        (SCHEDULER_TASK_STACK_SIZE_BYTES / sizeof(StackType_t))

typedef enum {
    MSG_COMPLETION = 1,
    MSG_DEVICE_MODE = 2
} sched_msg_type_t;

typedef struct {
    uint32_t request_id;
    uint8_t  type;
    uint8_t  slot;
    uint8_t  mode;
} sched_msg_t;

struct ems_poll_scheduler {
    modbus_master_handle_t master;
    ems_sched_core_t core;
    modbus_request_t request;              /* ~520 B: kept off the task stack */
    ems_poll_scheduler_stats_t snapshot;   /* read by other tasks under s_stats_mux */
    QueueHandle_t queue;
    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;
    uint32_t task_stack_size;
    volatile uint32_t queue_full;
    volatile bool stopping;
};

static struct ems_poll_scheduler s_scheduler;
static StaticQueue_t s_queue_cb;
static uint8_t s_queue_storage[SCHEDULER_STATIC_QUEUE_LEN * sizeof(sched_msg_t)]
    __attribute__((aligned(portBYTE_ALIGNMENT)));
static StaticSemaphore_t s_stopped_sem_cb;
static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[SCHEDULER_TASK_STACK_WORDS];
static bool s_in_use;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

static void handle_msg(struct ems_poll_scheduler *s, const sched_msg_t *m, int64_t now_us)
{
    if (m->type == MSG_COMPLETION) {
        const ems_sched_feedback_t fb = ems_sched_feedback(&s->core, m->request_id, now_us);
        if (fb == EMS_SCHED_FB_LATE) {
            ESP_LOGW(TAG, "late feedback request=0x%08" PRIX32 ": slot released, guard flag cleared",
                     m->request_id);
        } else if (fb == EMS_SCHED_FB_UNMATCHED) {
            ESP_LOGW(TAG, "feedback request=0x%08" PRIX32 " matches no pending request; ignored",
                     m->request_id);
        }
    } else if (m->type == MSG_DEVICE_MODE) {
        if (ems_sched_set_device_mode(&s->core, m->slot, (ems_sched_dev_mode_t)m->mode, now_us)) {
            ESP_LOGI(TAG, "device slot %u -> %s", (unsigned)m->slot,
                     m->mode == EMS_SCHED_DEV_OFFLINE ? "OFFLINE (probe mode)" : "ONLINE");
        } else {
            ESP_LOGW(TAG, "invalid device mode change slot=%u mode=%u", (unsigned)m->slot,
                     (unsigned)m->mode);
        }
    }
}

static void submit_due(struct ems_poll_scheduler *s, int64_t now_us)
{
    ems_sched_request_t r;
    while (ems_sched_next(&s->core, now_us, &r)) {
        modbus_request_t *req = &s->request;
        memset(req, 0, sizeof(*req));
        req->request_id = r.request_id;
        req->slave_addr = r.slave_id;
        req->function_code = r.function_code;
        req->start_register = r.start_address;
        req->register_count = r.count;
        req->timeout_ms = r.timeout_ms;
        req->max_retries = r.max_retries;
        req->allow_write_retries = false;

        const esp_err_t err = modbus_master_submit(s->master, req);
        ems_sched_submitted(&s->core, &r, err == ESP_OK, now_us);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "submit entry=%u request=0x%08" PRIX32 " slave=%u FC%02u %u+%u",
                     (unsigned)r.entry, r.request_id, (unsigned)r.slave_id,
                     (unsigned)r.function_code, (unsigned)r.start_address, (unsigned)r.count);
        } else {
            ESP_LOGW(TAG, "submit entry=%u rejected (%s); retry in %" PRIu32 " ms",
                     (unsigned)r.entry, esp_err_to_name(err), s->core.submit_retry_ms);
        }
    }
}

static void update_snapshot(struct ems_poll_scheduler *s)
{
    taskENTER_CRITICAL(&s_stats_mux);
    s->snapshot.core = s->core.stats;
    s->snapshot.entry_count = s->core.plan.entry_count;
    for (uint8_t e = 0; e < s->core.plan.entry_count; e++) {
        s->snapshot.entry[e] = s->core.entry[e].stats;
    }
    for (uint8_t d = 0; d < s->core.plan.device_count; d++) {
        s->snapshot.device_mode[d] = s->core.device[d].mode;
    }
    s->snapshot.health_flags = s->core.health_flags;
    s->snapshot.queue_full = s->queue_full;
    taskEXIT_CRITICAL(&s_stats_mux);
}

static void scheduler_task(void *argument)
{
    struct ems_poll_scheduler *s = argument;
    ems_stack_diag_t stack_diag;
    ems_stack_diag_init(&stack_diag, "ems_scheduler", s->task_stack_size);

    while (!s->stopping) {
        ems_stack_diag_sample(&stack_diag, TAG);

        /* Sleep until the next deadline (at most 50 ms); messages wake us early. */
        int64_t now_us = esp_timer_get_time();
        const int64_t next_us = ems_sched_next_event_us(&s->core);
        int64_t wait_ms = SCHEDULER_MAX_WAIT_MS;
        if (next_us != INT64_MAX) {
            const int64_t until = (next_us - now_us + 999) / 1000;
            wait_ms = until < 0 ? 0 : (until < wait_ms ? until : wait_ms);
        }
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)wait_ms);
        if (wait_ms > 0 && ticks == 0) {
            ticks = 1;
        }

        sched_msg_t msg;
        if (xQueueReceive(s->queue, &msg, ticks) == pdTRUE) {
            now_us = esp_timer_get_time();
            handle_msg(s, &msg, now_us);
            while (xQueueReceive(s->queue, &msg, 0) == pdTRUE) {
                handle_msg(s, &msg, now_us);
            }
        }

        now_us = esp_timer_get_time();
        const uint32_t expired = ems_sched_check_guards(&s->core, now_us);
        for (uint8_t d = 0; d < s->core.plan.device_count; d++) {
            if ((expired & (1u << d)) != 0u) {
                ESP_LOGE(TAG, "EVT_SCHED_GUARD_EXPIRED slot=%u request=0x%08" PRIX32
                              ": no feedback after %" PRId64 " ms; slot NOT released (Design 9.5), "
                              "HEALTH_SCHED_FEEDBACK raised",
                         (unsigned)d, s->core.device[d].pending_id,
                         s->core.plan.devices[d].guard_us / 1000);
            }
        }
        submit_due(s, now_us);
        update_snapshot(s);
    }

    xSemaphoreGive(s->stopped_sem);
    for (;;) {
        vTaskDelay(portMAX_DELAY);   /* owner suspends/deletes before reusing static storage */
    }
}

esp_err_t ems_poll_scheduler_start(const ems_poll_scheduler_config_t *config,
                                   ems_poll_scheduler_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL || config->master == NULL || config->plan == NULL ||
        config->plan->entry_count == 0U || config->plan->entry_count > EMS_SCHED_MAX_ENTRIES) {
        ESP_LOGE(TAG, "invalid scheduler arguments");
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (s_in_use) {
        ESP_LOGE(TAG, "scheduler is already running");
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t qlen = config->feedback_queue_length != 0U ? config->feedback_queue_length
                                                              : SCHEDULER_STATIC_QUEUE_LEN;
    const uint32_t stack_bytes = config->task_stack_size != 0U ? config->task_stack_size
                                                               : SCHEDULER_TASK_STACK_SIZE_BYTES;
    const UBaseType_t priority = config->task_priority != 0U ? config->task_priority : 5U;
    if (qlen > SCHEDULER_STATIC_QUEUE_LEN || stack_bytes > SCHEDULER_TASK_STACK_SIZE_BYTES ||
        stack_bytes < 2048U) {
        ESP_LOGE(TAG, "invalid queue length %u or stack size %" PRIu32, (unsigned)qlen, stack_bytes);
        return ESP_ERR_INVALID_ARG;
    }

    struct ems_poll_scheduler *s = &s_scheduler;
    memset(s, 0, sizeof(*s));
    s->master = config->master;
    s->task_stack_size = stack_bytes;
    ems_sched_init(&s->core, config->plan, config->submit_retry_delay_ms, esp_timer_get_time());
    update_snapshot(s);

    s->queue = xQueueCreateStatic(qlen, sizeof(sched_msg_t), s_queue_storage, &s_queue_cb);
    s->stopped_sem = xSemaphoreCreateBinaryStatic(&s_stopped_sem_cb);
    if (s->queue == NULL || s->stopped_sem == NULL) {
        memset(s, 0, sizeof(*s));
        return ESP_ERR_NO_MEM;
    }
    const UBaseType_t words = (UBaseType_t)((stack_bytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t));
    s->task = xTaskCreateStatic(scheduler_task, "ems_poll_sched", words, s, priority,
                                s_task_stack, &s_task_tcb);
    if (s->task == NULL) {
        vQueueDelete(s->queue);
        vSemaphoreDelete(s->stopped_sem);
        memset(s, 0, sizeof(*s));
        return ESP_ERR_NO_MEM;
    }

    s_in_use = true;
    *out_handle = s;
    ESP_LOGI(TAG, "scheduler started: %u poll entries, %u devices",
             (unsigned)config->plan->entry_count, (unsigned)config->plan->device_count);
    for (uint8_t e = 0; e < config->plan->entry_count; e++) {
        const ems_sched_entry_t *en = &config->plan->entries[e];
        ESP_LOGI(TAG, "  entry %u: slot %u slave %u group %u FC%02u %u+%u every %" PRIu32
                      " ms (offset %" PRIu32 " ms)%s",
                 (unsigned)e, (unsigned)en->slot, (unsigned)en->slave_id, (unsigned)en->group_index,
                 (unsigned)en->function_code, (unsigned)en->start_address, (unsigned)en->count,
                 en->interval_ms, en->offset_ms, en->is_probe ? " [probe]" : "");
    }
    return ESP_OK;
}

static esp_err_t send_msg(ems_poll_scheduler_handle_t handle, const sched_msg_t *msg)
{
    if (xQueueSend(handle->queue, msg, 0) != pdTRUE) {
        handle->queue_full++;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t ems_poll_scheduler_notify_completion(ems_poll_scheduler_handle_t handle, uint32_t request_id)
{
    if (handle == NULL || request_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const sched_msg_t msg = { .request_id = request_id, .type = MSG_COMPLETION };
    return send_msg(handle, &msg);
}

esp_err_t ems_poll_scheduler_set_device_mode(ems_poll_scheduler_handle_t handle, uint8_t slot,
                                             ems_sched_dev_mode_t mode)
{
    if (handle == NULL || slot >= EMS_SCHED_MAX_DEVICES ||
        (mode != EMS_SCHED_DEV_ONLINE && mode != EMS_SCHED_DEV_OFFLINE)) {
        return ESP_ERR_INVALID_ARG;
    }
    const sched_msg_t msg = { .type = MSG_DEVICE_MODE, .slot = slot, .mode = (uint8_t)mode };
    return send_msg(handle, &msg);
}

esp_err_t ems_poll_scheduler_get_stats(ems_poll_scheduler_handle_t handle, ems_poll_scheduler_stats_t *out)
{
    if (handle == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_stats_mux);
    *out = handle->snapshot;
    taskEXIT_CRITICAL(&s_stats_mux);
    return ESP_OK;
}

esp_err_t ems_poll_scheduler_stop(ems_poll_scheduler_handle_t handle)
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
    vQueueDelete(handle->queue);
    vSemaphoreDelete(handle->stopped_sem);
    memset(handle, 0, sizeof(*handle));
    s_in_use = false;
    return ESP_OK;
}
