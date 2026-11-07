#pragma once

/*
 * MyEMS acquisition task. Sole consumer of the Modbus completion queue.
 *
 * DP-WP4 D5: results are routed by request ID (entry = id >> 24, Design 9.1)
 * and checked against their poll entry; every result is fed back to the
 * scheduler. Decoding, quality and the state store are added in D6.
 */

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ems_config_types.h"
#include "ems_poll_scheduler.h"
#include "ems_sched_core.h"
#include "modbus_rtu_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ems_acquisition *ems_acquisition_handle_t;

typedef struct {
    modbus_master_handle_t master;
    ems_poll_scheduler_handle_t scheduler;
    const ems_sched_plan_t *plan;        /* same plan as the scheduler (not copied) */
    const ems_config_build_t *config;    /* generated tables, for names            */
    UBaseType_t task_priority;           /* 0 = 6                                   */
    uint32_t task_stack_size;            /* bytes, 0 = 4096                         */
    uint32_t completion_wait_ms;         /* 0 = 100                                 */
} ems_acquisition_config_t;

typedef struct {
    uint32_t results;
    uint32_t ok;
    uint32_t failed;              /* TIMEOUT, CRC, PROTOCOL, TRANSPORT, EXCEPTION */
    uint32_t cancelled;
    uint32_t routing_errors;      /* unknown entry or slave/FC/count mismatch     */
    uint32_t feedback_failures;   /* scheduler queue full                          */
} ems_acquisition_stats_t;

esp_err_t ems_acquisition_start(const ems_acquisition_config_t *config,
                                ems_acquisition_handle_t *out_handle);

esp_err_t ems_acquisition_get_stats(ems_acquisition_handle_t handle, ems_acquisition_stats_t *out);

/* Stop the scheduler first so no new requests are submitted, then stop acquisition. */
esp_err_t ems_acquisition_stop(ems_acquisition_handle_t handle);

/* Text for a Modbus result status (never NULL). */
const char *ems_acquisition_status_name(modbus_result_status_t status);

#ifdef __cplusplus
}
#endif
