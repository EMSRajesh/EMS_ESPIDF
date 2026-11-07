#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ems_poll_scheduler.h"
#include "modbus_rtu_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ems_acquisition *ems_acquisition_handle_t;

typedef struct {
    modbus_master_handle_t master;
    ems_poll_scheduler_handle_t scheduler;
    const ems_meter_profile_t *meters;
    size_t meter_count;
    UBaseType_t task_priority;
    uint32_t task_stack_size;
    uint32_t completion_wait_ms;
} ems_acquisition_config_t;

/* Starts the task that consumes Modbus completions and updates/logs measurements.
 * Meter profiles are copied; name pointers must remain valid for the task lifetime. */
esp_err_t ems_acquisition_start(const ems_acquisition_config_t *config,
                                ems_acquisition_handle_t *out_handle);

/* Stop the scheduler first so no new requests are submitted, then stop acquisition. */
esp_err_t ems_acquisition_stop(ems_acquisition_handle_t handle);

#ifdef __cplusplus
}
#endif
