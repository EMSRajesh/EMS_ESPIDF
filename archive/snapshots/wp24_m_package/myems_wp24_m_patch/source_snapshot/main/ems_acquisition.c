#include "ems_acquisition.h"
#include "ems_stack_diag.h"

#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ems_acquisition";

typedef struct {
    const char *name;
    uint8_t slave_addr;
} meter_name_t;

struct ems_acquisition {
    modbus_master_handle_t master;
    ems_poll_scheduler_handle_t scheduler;
    size_t meter_count;
    meter_name_t meters[EMS_SCHEDULER_MAX_METERS];
    UBaseType_t task_priority;
    uint32_t task_stack_size;
    uint32_t completion_wait_ms;
    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;
    volatile bool stopping;
};

#define ACQUISITION_TASK_STACK_SIZE_BYTES 4096U
#define ACQUISITION_TASK_STACK_WORDS \
    (ACQUISITION_TASK_STACK_SIZE_BYTES / sizeof(StackType_t))
static struct ems_acquisition s_acquisition;
static StaticSemaphore_t s_acquisition_stopped_sem_cb;
static StaticTask_t s_acquisition_task_tcb;
static StackType_t s_acquisition_task_stack[ACQUISITION_TASK_STACK_WORDS];
static bool s_acquisition_in_use;

static const char *meter_name_for_slave(const struct ems_acquisition *acq,
                                        uint8_t slave_addr)
{
    for (size_t i = 0; i < acq->meter_count; ++i) {
        if (acq->meters[i].slave_addr == slave_addr) {
            return acq->meters[i].name;
        }
    }
    return "unknown_meter";
}

static void process_result(struct ems_acquisition *acq,
                           const modbus_result_t *result)
{
    const char *meter_name = meter_name_for_slave(acq, result->slave_addr);

    if (result->status == MODBUS_RESULT_OK) {
        ESP_LOGI(TAG,
                 "%s request=%" PRIu32 " slave=%u FC%u OK registers=%u retries=%u",
                 meter_name, result->request_id, result->slave_addr,
                 result->function_code, result->register_count, result->retry_count);

        for (uint16_t i = 0; i < result->register_count; ++i) {
            ESP_LOGI(TAG, "%s register[%u]=%u (0x%04X)",
                     meter_name, i, result->registers[i], result->registers[i]);
        }
        /* Production integration point:
         * Decode raw register words using the selected meter profile, apply
         * scaling/endianness, validate engineering ranges, timestamp the sample,
         * and publish a typed measurement to the EMS data model. Do not assume
         * that register zero is voltage; register maps are device-specific. */
    } else {
        ESP_LOGW(TAG,
                 "%s request=%" PRIu32 " failed status=%d exception=0x%02X retries=%u",
                 meter_name, result->request_id, (int)result->status,
                 result->exception_code, result->retry_count);
    }

    /* Let the scheduler release this meter's in-flight slot. Notify for both
     * success and failure because either outcome terminates the request. */
    esp_err_t notify_err = ems_poll_scheduler_notify_completion(
        acq->scheduler, result->request_id);
    if (notify_err != ESP_OK) {
        ESP_LOGE(TAG, "scheduler completion feedback failed for request=%" PRIu32
                      ": %s", result->request_id, esp_err_to_name(notify_err));
    }
}

static void acquisition_task(void *argument)
{
    struct ems_acquisition *acq = argument;
    modbus_result_t result;
    const uint32_t allocated_stack_bytes = acq->task_stack_size != 0U
        ? acq->task_stack_size : ACQUISITION_TASK_STACK_SIZE_BYTES;
    ems_stack_diag_t stack_diag;
    ems_stack_diag_init(&stack_diag, "ems_acquisition", allocated_stack_bytes);

    while (!acq->stopping) {
        ems_stack_diag_sample(&stack_diag, TAG);
        esp_err_t err = modbus_master_get_completion(
            acq->master, &result, pdMS_TO_TICKS(acq->completion_wait_ms));
        if (err == ESP_OK) {
            process_result(acq, &result);
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "completion read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    xSemaphoreGive(acq->stopped_sem);
    for (;;) vTaskDelay(portMAX_DELAY); /* Owner suspends/deletes before reusing static storage. */
}

esp_err_t ems_acquisition_start(const ems_acquisition_config_t *config,
                                ems_acquisition_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL || config->master == NULL ||
        config->scheduler == NULL || config->meters == NULL ||
        config->meter_count == 0U ||
        config->meter_count > EMS_SCHEDULER_MAX_METERS) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (s_acquisition_in_use ||
        (config->task_stack_size != 0U &&
         config->task_stack_size > ACQUISITION_TASK_STACK_SIZE_BYTES)) {
        return ESP_ERR_INVALID_ARG;
    }

    struct ems_acquisition *acq = &s_acquisition;
    memset(acq, 0, sizeof(*acq));
    acq->master = config->master;
    acq->scheduler = config->scheduler;
    acq->meter_count = config->meter_count;
    acq->task_priority = config->task_priority != 0U ? config->task_priority : 6U;
    acq->task_stack_size = config->task_stack_size != 0U ? config->task_stack_size : 4096U;
    acq->completion_wait_ms = config->completion_wait_ms != 0U
        ? config->completion_wait_ms : 100U;

    for (size_t i = 0; i < config->meter_count; ++i) {
        acq->meters[i].name = config->meters[i].name;
        acq->meters[i].slave_addr = config->meters[i].slave_addr;
    }

    acq->stopped_sem = xSemaphoreCreateBinaryStatic(&s_acquisition_stopped_sem_cb);
    if (acq->stopped_sem == NULL) return ESP_ERR_NO_MEM;

    UBaseType_t stack_depth_words = (UBaseType_t)(
        (acq->task_stack_size + sizeof(StackType_t) - 1U) / sizeof(StackType_t));
    if (stack_depth_words == 0U ||
        stack_depth_words > ACQUISITION_TASK_STACK_WORDS ||
        acq->task_priority == 0U) {
        vSemaphoreDelete(acq->stopped_sem);
        memset(acq, 0, sizeof(*acq));
        return ESP_ERR_INVALID_ARG;
    }
    acq->task = xTaskCreateStatic(acquisition_task, "ems_acquisition",
                                  stack_depth_words, acq,
                                  acq->task_priority, s_acquisition_task_stack,
                                  &s_acquisition_task_tcb);
    if (acq->task == NULL) {
        vSemaphoreDelete(acq->stopped_sem);
        memset(acq, 0, sizeof(*acq));
        return ESP_ERR_NO_MEM;
    }

    s_acquisition_in_use = true;
    *out_handle = acq;
    ESP_LOGI(TAG, "acquisition task started for %u meter profiles",
             (unsigned)acq->meter_count);
    return ESP_OK;
}

esp_err_t ems_acquisition_stop(ems_acquisition_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    handle->stopping = true;
    if (xSemaphoreTake(handle->stopped_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    vTaskSuspend(handle->task);
    vTaskDelete(handle->task);
    vSemaphoreDelete(handle->stopped_sem);
    memset(handle, 0, sizeof(*handle));
    s_acquisition_in_use = false;
    return ESP_OK;
}
