#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "modbus_rtu_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMS_SCHEDULER_MAX_METERS 4U

typedef struct ems_poll_scheduler *ems_poll_scheduler_handle_t;

typedef struct {
    const char *name;             /* Static name used only for diagnostics. */
    uint8_t slave_addr;
    uint8_t function_code;        /* Currently FC03 or FC04. */
    uint16_t start_register;      /* Device-specific; verify meter manual. */
    uint16_t register_count;
    uint32_t poll_period_ms;
    uint32_t initial_offset_ms;   /* Spreads requests across a shared bus. */
    uint32_t timeout_ms;
    uint8_t max_retries;
} ems_meter_profile_t;

typedef struct {
    modbus_master_handle_t master;
    const ems_meter_profile_t *meters;
    size_t meter_count;
    UBaseType_t task_priority;
    uint32_t task_stack_size;
    uint16_t feedback_queue_length;
    uint32_t submit_retry_delay_ms;
} ems_poll_scheduler_config_t;

/* Starts the periodic scheduler. Meter profiles are copied; name pointers must
 * refer to static or otherwise long-lived strings. */
esp_err_t ems_poll_scheduler_start(const ems_poll_scheduler_config_t *config,
                                   ems_poll_scheduler_handle_t *out_handle);

/* Called by the acquisition task after it consumes a terminal Modbus result.
 * Non-blocking: returns ESP_ERR_TIMEOUT if the feedback queue is full. */
esp_err_t ems_poll_scheduler_notify_completion(
    ems_poll_scheduler_handle_t handle,
    uint32_t request_id);

/* Stops the scheduler task and releases its resources. Stop acquisition first. */
esp_err_t ems_poll_scheduler_stop(ems_poll_scheduler_handle_t handle);

#ifdef __cplusplus
}
#endif
