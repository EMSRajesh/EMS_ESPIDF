/*
 * MyEMS acquisition task (DP-WP4 milestone D5 state).
 *
 * Changes from 09_057:
 *   - the meter is no longer looked up by slave address (which cannot tell
 *     the three poll groups of one meter apart); the result is routed by its
 *     request ID (entry index in bits 31..24, Design 9.1);
 *   - the result's slave address and function code, and for OK results the
 *     register count, must match the entry, otherwise it is a routing error:
 *     counted, logged, discarded (feedback is still sent);
 *   - one INFO line per result instead of one line per register.
 * D6 adds decoding, quality and the state store at the marked place.
 */
#include "ems_acquisition.h"
#include "ems_stack_diag.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ems_acquisition";

#define ACQ_TASK_STACK_SIZE_BYTES 4096U
#define ACQ_TASK_STACK_WORDS      (ACQ_TASK_STACK_SIZE_BYTES / sizeof(StackType_t))
#define ACQ_LOG_MAX_REGS          20U

struct ems_acquisition {
    modbus_master_handle_t master;
    ems_poll_scheduler_handle_t scheduler;
    const ems_sched_plan_t *plan;
    const ems_config_build_t *config;
    uint32_t task_stack_size;
    uint32_t completion_wait_ms;
    UBaseType_t task_priority;
    ems_acquisition_stats_t stats;
    modbus_result_t result;                 /* ~520 B: kept off the task stack */
    char line[ACQ_LOG_MAX_REGS * 5U + 8U];  /* hex dump buffer                 */
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

static void count(uint32_t *field)
{
    taskENTER_CRITICAL(&s_stats_mux);
    (*field)++;
    taskEXIT_CRITICAL(&s_stats_mux);
}

static void process_result(struct ems_acquisition *acq, const modbus_result_t *r)
{
    count(&acq->stats.results);
    const uint8_t e = ems_sched_request_entry(r->request_id);
    const ems_sched_entry_t *en = (e < acq->plan->entry_count) ? &acq->plan->entries[e] : NULL;

    bool routed = (en != NULL) && r->slave_addr == en->slave_id && r->function_code == en->function_code;
    if (routed && r->status == MODBUS_RESULT_OK && r->register_count != en->count) {
        routed = false;
    }

    if (!routed) {
        count(&acq->stats.routing_errors);
        ESP_LOGW(TAG, "routing error request=0x%08" PRIX32 " entry=%u slave=%u FC%02u regs=%u status=%s: "
                      "does not match its poll entry; discarded",
                 r->request_id, (unsigned)e, (unsigned)r->slave_addr, (unsigned)r->function_code,
                 (unsigned)r->register_count, ems_acquisition_status_name(r->status));
    } else if (r->status == MODBUS_RESULT_OK) {
        count(&acq->stats.ok);
        size_t pos = 0;
        const uint16_t n = r->register_count < ACQ_LOG_MAX_REGS ? r->register_count : ACQ_LOG_MAX_REGS;
        acq->line[0] = '\0';
        for (uint16_t i = 0; i < n && pos + 6U < sizeof(acq->line); i++) {
            pos += (size_t)snprintf(&acq->line[pos], sizeof(acq->line) - pos, " %04X", r->registers[i]);
        }
        ESP_LOGI(TAG, "%s grp%u FC%02u %u+%u OK request=0x%08" PRIX32 " retries=%u:%s%s",
                 device_name(acq, en->slot), (unsigned)en->group_index, (unsigned)en->function_code,
                 (unsigned)en->start_address, (unsigned)en->count, r->request_id,
                 (unsigned)r->retry_count, acq->line, r->register_count > n ? " ..." : "");
        /* D6: decode with the entry's mappings, assign quality, publish to the state store. */
    } else if (r->status == MODBUS_RESULT_CANCELLED) {
        count(&acq->stats.cancelled);
        ESP_LOGI(TAG, "%s grp%u request=0x%08" PRIX32 " CANCELLED",
                 device_name(acq, en->slot), (unsigned)en->group_index, r->request_id);
    } else {
        count(&acq->stats.failed);
        ESP_LOGW(TAG, "%s grp%u FC%02u %u+%u %s request=0x%08" PRIX32 " exception=0x%02X retries=%u",
                 device_name(acq, en->slot), (unsigned)en->group_index, (unsigned)en->function_code,
                 (unsigned)en->start_address, (unsigned)en->count, ems_acquisition_status_name(r->status),
                 r->request_id, (unsigned)r->exception_code, (unsigned)r->retry_count);
    }

    /* Every terminal result, including routing errors, releases the scheduler slot. */
    if (ems_poll_scheduler_notify_completion(acq->scheduler, r->request_id) != ESP_OK) {
        count(&acq->stats.feedback_failures);
        ESP_LOGE(TAG, "scheduler feedback failed for request=0x%08" PRIX32, r->request_id);
    }
}

static void acquisition_task(void *argument)
{
    struct ems_acquisition *acq = argument;
    ems_stack_diag_t stack_diag;
    ems_stack_diag_init(&stack_diag, "ems_acquisition", acq->task_stack_size);

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
    }

    xSemaphoreGive(acq->stopped_sem);
    for (;;) {
        vTaskDelay(portMAX_DELAY);   /* owner suspends/deletes before reusing static storage */
    }
}

esp_err_t ems_acquisition_start(const ems_acquisition_config_t *config, ems_acquisition_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL || config->master == NULL || config->scheduler == NULL ||
        config->plan == NULL || config->plan->entry_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    const uint32_t stack_bytes = config->task_stack_size != 0U ? config->task_stack_size
                                                               : ACQ_TASK_STACK_SIZE_BYTES;
    if (s_in_use || stack_bytes > ACQ_TASK_STACK_SIZE_BYTES || stack_bytes < 2048U) {
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
    ESP_LOGI(TAG, "acquisition task started for %u poll entries", (unsigned)acq->plan->entry_count);
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
