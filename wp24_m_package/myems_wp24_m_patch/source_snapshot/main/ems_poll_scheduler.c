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
#define SCHEDULER_LOOP_WAIT_MS 50U
#define PENDING_GUARD_MS       5000U

typedef struct {
    uint32_t request_id;
} completion_feedback_t;

typedef struct {
    ems_meter_profile_t profile;
    bool pending;
    uint32_t pending_request_id;
    int64_t pending_deadline_us;
    int64_t next_due_us;
} meter_slot_t;

struct ems_poll_scheduler {
    modbus_master_handle_t master;
    size_t meter_count;
    meter_slot_t meters[EMS_SCHEDULER_MAX_METERS];
    QueueHandle_t feedback_queue;
    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;
    uint32_t next_request_id;
    uint32_t submit_retry_delay_ms;
    uint32_t task_stack_size;
    volatile bool stopping;
};

#define SCHEDULER_STATIC_FEEDBACK_Q 16U
#define SCHEDULER_TASK_STACK_SIZE_BYTES 4096U
#define SCHEDULER_TASK_STACK_WORDS \
    (SCHEDULER_TASK_STACK_SIZE_BYTES / sizeof(StackType_t))
static struct ems_poll_scheduler s_scheduler;
static StaticQueue_t s_feedback_queue_cb;
static uint8_t s_feedback_queue_storage[SCHEDULER_STATIC_FEEDBACK_Q * sizeof(completion_feedback_t)] __attribute__((aligned(portBYTE_ALIGNMENT)));
static StaticSemaphore_t s_scheduler_stopped_sem_cb;
static StaticTask_t s_scheduler_task_tcb;
static StackType_t s_scheduler_task_stack[SCHEDULER_TASK_STACK_SIZE_BYTES / sizeof(StackType_t)];
static bool s_scheduler_in_use;

static int64_t ms_to_us(uint32_t milliseconds)
{
    return (int64_t)milliseconds * 1000LL;
}

static void consume_feedback(struct ems_poll_scheduler *scheduler)
{
    completion_feedback_t feedback;
    while (xQueueReceive(scheduler->feedback_queue, &feedback, 0) == pdTRUE) {
        for (size_t i = 0; i < scheduler->meter_count; ++i) {
            meter_slot_t *slot = &scheduler->meters[i];
            if (slot->pending && slot->pending_request_id == feedback.request_id) {
                ESP_LOGD(TAG, "%s completed request=%" PRIu32,
                         slot->profile.name, feedback.request_id);
                slot->pending = false;
                slot->pending_request_id = 0U;
                slot->pending_deadline_us = 0;
                break;
            }
        }
    }
}

static void advance_deadline(meter_slot_t *slot, int64_t now_us)
{
    const int64_t period_us = ms_to_us(slot->profile.poll_period_ms);
    /* Skip missed periods rather than flooding the bus with catch-up requests. */
    do {
        slot->next_due_us += period_us;
    } while (slot->next_due_us <= now_us);
}

static void scheduler_task(void *argument)
{
    struct ems_poll_scheduler *scheduler = argument;
    const uint32_t allocated_stack_bytes = scheduler->task_stack_size != 0U
        ? scheduler->task_stack_size : SCHEDULER_TASK_STACK_SIZE_BYTES;
    ems_stack_diag_t stack_diag;
    ems_stack_diag_init(&stack_diag, "ems_scheduler", allocated_stack_bytes);

    while (!scheduler->stopping) {
        ems_stack_diag_sample(&stack_diag, TAG);
        /* Feedback wakes this task early when a request completes. */
        completion_feedback_t feedback;
        if (xQueueReceive(scheduler->feedback_queue, &feedback,
                          pdMS_TO_TICKS(SCHEDULER_LOOP_WAIT_MS)) == pdTRUE) {
            for (size_t i = 0; i < scheduler->meter_count; ++i) {
                meter_slot_t *slot = &scheduler->meters[i];
                if (slot->pending && slot->pending_request_id == feedback.request_id) {
                    ESP_LOGD(TAG, "%s completed request=%" PRIu32,
                             slot->profile.name, feedback.request_id);
                    slot->pending = false;
                    slot->pending_request_id = 0U;
                    slot->pending_deadline_us = 0;
                    break;
                }
            }
            consume_feedback(scheduler);
        }

        int64_t now_us = esp_timer_get_time();
        for (size_t i = 0; i < scheduler->meter_count; ++i) {
            meter_slot_t *slot = &scheduler->meters[i];

            /* Defensive recovery if a completion event was lost. This does not
             * cancel a transaction in the master; it only prevents a scheduler
             * slot remaining pending forever. Queue sizing should make this rare. */
            if (slot->pending && now_us >= slot->pending_deadline_us) {
                ESP_LOGW(TAG, "%s request=%" PRIu32
                         " completion feedback overdue; releasing scheduler slot",
                         slot->profile.name, slot->pending_request_id);
                slot->pending = false;
                slot->pending_request_id = 0U;
                slot->pending_deadline_us = 0;
                slot->next_due_us = now_us + ms_to_us(scheduler->submit_retry_delay_ms);
            }

            if (now_us < slot->next_due_us) {
                continue;
            }

            if (slot->pending) {
                /* A prior request is still active. Skip this polling period. */
                advance_deadline(slot, now_us);
                continue;
            }

            uint32_t request_id = scheduler->next_request_id++;
            if (scheduler->next_request_id == 0U) {
                scheduler->next_request_id = 1U; /* Reserve zero as invalid. */
            }

            modbus_request_t request = {
                .request_id = request_id,
                .slave_addr = slot->profile.slave_addr,
                .function_code = slot->profile.function_code,
                .start_register = slot->profile.start_register,
                .register_count = slot->profile.register_count,
                .timeout_ms = slot->profile.timeout_ms,
                .max_retries = slot->profile.max_retries
            };

            /* Mark the slot before submit so an unusually fast completion cannot
             * race ahead of the pending-state update. Roll back on rejection. */
            slot->pending = true;
            slot->pending_request_id = request_id;
            const uint32_t retry_limit =
                slot->profile.max_retries == MODBUS_RETRIES_USE_DEFAULT
                    ? MODBUS_MAX_RETRIES
                    : slot->profile.max_retries;
            const int64_t transaction_budget_ms =
                ((int64_t)slot->profile.timeout_ms * ((int64_t)retry_limit + 1LL)) +
                ((int64_t)retry_limit * 100LL) + PENDING_GUARD_MS;
            slot->pending_deadline_us = now_us + transaction_budget_ms * 1000LL;

            esp_err_t err = modbus_master_submit(scheduler->master, &request);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "scheduled %s: request=%" PRIu32 " slave=%u FC%u",
                         slot->profile.name, request_id,
                         slot->profile.slave_addr, slot->profile.function_code);
                advance_deadline(slot, now_us);
            } else {
                ESP_LOGW(TAG, "submit %s rejected: %s",
                         slot->profile.name, esp_err_to_name(err));
                slot->pending = false;
                slot->pending_request_id = 0U;
                slot->pending_deadline_us = 0;
                slot->next_due_us = now_us + ms_to_us(scheduler->submit_retry_delay_ms);
            }
        }
    }

    xSemaphoreGive(scheduler->stopped_sem);
    for (;;) vTaskDelay(portMAX_DELAY); /* Owner suspends/deletes before reusing static storage. */
}

esp_err_t ems_poll_scheduler_start(const ems_poll_scheduler_config_t *config,
                                   ems_poll_scheduler_handle_t *out_handle)
{
    if (config == NULL ||
        out_handle == NULL ||
        config->master == NULL ||
        config->meters == NULL ||
        config->meter_count == 0U ||
        config->meter_count > EMS_SCHEDULER_MAX_METERS) {
        ESP_LOGE(TAG, "Invalid scheduler arguments");
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = NULL;

    if (s_scheduler_in_use) {
        ESP_LOGE(TAG, "Scheduler is already running");
        return ESP_ERR_INVALID_STATE;
    }

    if (config->feedback_queue_length > SCHEDULER_STATIC_FEEDBACK_Q) {
        ESP_LOGE(TAG, "Invalid feedback queue length: %u",
                (unsigned)config->feedback_queue_length);
        return ESP_ERR_INVALID_ARG;
    }

    if (config->task_stack_size > SCHEDULER_TASK_STACK_SIZE_BYTES) {
        ESP_LOGE(TAG, "Requested stack size %lu exceeds static capacity %u bytes",
                (unsigned long)config->task_stack_size,
                (unsigned)SCHEDULER_TASK_STACK_SIZE_BYTES);
        return ESP_ERR_INVALID_ARG;
    }

    struct ems_poll_scheduler *scheduler = &s_scheduler;
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->master = config->master;
    scheduler->meter_count = config->meter_count;
    scheduler->next_request_id = 1U;
    scheduler->task_stack_size = config->task_stack_size;
    scheduler->submit_retry_delay_ms = config->submit_retry_delay_ms != 0U
        ? config->submit_retry_delay_ms : 250U;

    int64_t now_us = esp_timer_get_time();
    for (size_t i = 0; i < config->meter_count; ++i) {
        const ems_meter_profile_t *profile = &config->meters[i];
        if (profile->name == NULL || profile->slave_addr == 0U ||
            profile->slave_addr > 247U ||
            (profile->function_code != 0x03U && profile->function_code != 0x04U) ||
            profile->register_count == 0U ||
            profile->register_count > MODBUS_MAX_REGISTERS ||
            profile->poll_period_ms == 0U || profile->timeout_ms == 0U ||
            (profile->max_retries != MODBUS_RETRIES_USE_DEFAULT &&
             profile->max_retries > MODBUS_MAX_RETRIES) ||
            (uint32_t)profile->start_register + profile->register_count > 65536UL) {
            memset(scheduler, 0, sizeof(*scheduler));
            return ESP_ERR_INVALID_ARG;
        }
        /* Two profiles on one multidrop bus must not use the same slave ID. */
        for (size_t previous = 0; previous < i; ++previous) {
            if (config->meters[previous].slave_addr == profile->slave_addr) {
                ESP_LOGE(TAG, "duplicate slave address %u in meter profiles",
                         profile->slave_addr);
                memset(scheduler, 0, sizeof(*scheduler));
                return ESP_ERR_INVALID_ARG;
            }
        }
        scheduler->meters[i].profile = *profile;
        scheduler->meters[i].next_due_us = now_us + ms_to_us(profile->initial_offset_ms);
    }

    UBaseType_t priority = config->task_priority != 0U ? config->task_priority : 5U;
    uint32_t stack_size_bytes = config->task_stack_size != 0U
        ? config->task_stack_size
        : SCHEDULER_TASK_STACK_SIZE_BYTES;
    UBaseType_t stack_depth_words = (UBaseType_t)(
        (stack_size_bytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t));
    uint16_t feedback_length = config->feedback_queue_length != 0U
        ? config->feedback_queue_length
        : SCHEDULER_STATIC_FEEDBACK_Q;

    if (stack_depth_words == 0U ||
        stack_depth_words > SCHEDULER_TASK_STACK_WORDS ||
        priority == 0U) {
        ESP_LOGE(TAG, "Invalid scheduler task config: stack_bytes=%" PRIu32
                      " stack_words=%u priority=%u",
                 stack_size_bytes, (unsigned)stack_depth_words, (unsigned)priority);
        memset(scheduler, 0, sizeof(*scheduler));
        return ESP_ERR_INVALID_ARG;
    }

    scheduler->feedback_queue = xQueueCreateStatic(
        feedback_length,
        sizeof(completion_feedback_t),
        s_feedback_queue_storage,
        &s_feedback_queue_cb);
    scheduler->stopped_sem =
        xSemaphoreCreateBinaryStatic(&s_scheduler_stopped_sem_cb);

    if (scheduler->feedback_queue == NULL || scheduler->stopped_sem == NULL) {
        if (scheduler->feedback_queue != NULL) {
            vQueueDelete(scheduler->feedback_queue);
        }
        if (scheduler->stopped_sem != NULL) {
            vSemaphoreDelete(scheduler->stopped_sem);
        }
        memset(scheduler, 0, sizeof(*scheduler));
        return ESP_ERR_NO_MEM;
    }

    scheduler->task = xTaskCreateStatic(
        scheduler_task,
        "ems_poll_sched",
        stack_depth_words,
        scheduler,
        priority,
        s_scheduler_task_stack,
        &s_scheduler_task_tcb);

    if (scheduler->task == NULL) {
        vQueueDelete(scheduler->feedback_queue);
        vSemaphoreDelete(scheduler->stopped_sem);
        memset(scheduler, 0, sizeof(*scheduler));
        return ESP_ERR_NO_MEM;
    }

    s_scheduler_in_use = true;
    *out_handle = scheduler;
    ESP_LOGI(TAG, "scheduler started with %u meter profiles",
             (unsigned)scheduler->meter_count);
    return ESP_OK;
}

esp_err_t ems_poll_scheduler_notify_completion(ems_poll_scheduler_handle_t handle,
                                              uint32_t request_id)
{
    if (handle == NULL || request_id == 0U) return ESP_ERR_INVALID_ARG;
    completion_feedback_t feedback = {.request_id = request_id};
    if (xQueueSend(handle->feedback_queue, &feedback, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t ems_poll_scheduler_stop(ems_poll_scheduler_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    handle->stopping = true;
    if (xSemaphoreTake(handle->stopped_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    vTaskSuspend(handle->task);
    vTaskDelete(handle->task);
    vQueueDelete(handle->feedback_queue);
    vSemaphoreDelete(handle->stopped_sem);
    memset(handle, 0, sizeof(*handle));
    s_scheduler_in_use = false;
    return ESP_OK;
}
