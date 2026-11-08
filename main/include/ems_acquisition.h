#pragma once

/*
 * MyEMS acquisition task. Sole consumer of the Modbus completion queue and
 * sole writer of the measurement state store.
 *
 * DP-WP4 D6: every terminal result is routed by request ID, decoded with the
 * generated mapping table and published to the state store with quality,
 * following the Design 9.3 table (logic in ems_acq_core.c, host-tested).
 * GOOD values older than their stale timeout become STALE (freshness check
 * on every wake-up). Every result is fed back to the scheduler.
 *
 * Readers (any task) use ems_state_get() / ems_state_snapshot_device().
 *
 * DP-WP4 D7: device communication state (Design 9.4). After
 * comm_lost_threshold consecutive failed transactions a device goes OFFLINE
 * (all parameters COMM_LOST, scheduler probe mode); the first answer brings
 * it back ONLINE. Events go to the event ring (ems_event.h) and the log.
 *
 * DP-WP4 D8: bus health (Design 9.7, ems_bus_health.h). Every loop the task
 * reads the transport recovery state and the master counters. On BUS_FAULT:
 * EVT_BUS_FAULT, the scheduler holds polling and cancels what is still in the
 * master, every device on the bus goes OFFLINE (COMM_LOST). On BUS_OK:
 * EVT_BUS_OK, the scheduler probes; devices come back through probing.
 */

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ems_acq_core.h"
#include "ems_bus_health.h"
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
    rs485_handle_t transport;            /* bus 0; NULL = no bus health check (D8)    */
    ems_poll_scheduler_handle_t scheduler;
    const ems_sched_plan_t *plan;        /* same plan as the scheduler (not copied)  */
    const ems_config_build_t *config;    /* generated tables                          */
    UBaseType_t task_priority;           /* 0 = 6                                     */
    uint32_t task_stack_size;            /* bytes, 0 = 6144                           */
    uint32_t completion_wait_ms;         /* 0 = 100 (also the freshness check period) */
    uint32_t value_dump_interval_ms;     /* 0 = no periodic VAL dump                  */
} ems_acquisition_config_t;

typedef struct {
    uint32_t results;
    uint32_t ok;
    uint32_t failed;              /* TIMEOUT, CRC, PROTOCOL, TRANSPORT, EXCEPTION */
    uint32_t cancelled;
    uint32_t routing_errors;      /* unknown entry, slave/FC mismatch, wrong count */
    uint32_t feedback_failures;   /* scheduler queue full                           */
    uint32_t stale_transitions;   /* GOOD -> STALE by the freshness check          */
    /* bus health (Design 9.7, D8) */
    uint8_t  bus_state;           /* ems_bus_state_t: HEALTH_BUS_FAULT when BUS_FAULT */
    uint8_t  bus_reason;          /* ems_bus_reason_t of the current / last fault     */
    uint8_t  bus_transport;       /* ems_bus_transport_t, last read                   */
    uint32_t bus_faults;
    uint32_t bus_recoveries;
    /* resources (D9, Test Spec PERF-004/006) */
    uint32_t proc_elec_last_us;   /* ems_acq_core_process time, electrical group (group 0) */
    uint32_t proc_elec_max_us;
    uint32_t proc_max_us;         /* any result                                       */
    uint32_t master_outstanding_hwm;  /* max accepted - completed: request queue + active */
    uint32_t completion_backlog_hwm;  /* max completed - consumed: completion queue      */
} ems_acquisition_stats_t;

/* Initialises the state store from the configuration and starts the task. */
esp_err_t ems_acquisition_start(const ems_acquisition_config_t *config,
                                ems_acquisition_handle_t *out_handle);

esp_err_t ems_acquisition_get_stats(ems_acquisition_handle_t handle, ems_acquisition_stats_t *out);

/* Per-device counters (Design 9.9, acquisition part). */
esp_err_t ems_acquisition_get_device_stats(ems_acquisition_handle_t handle, uint8_t slot,
                                           ems_acq_device_stats_t *out);

/* Stop the scheduler first so no new requests are submitted, then stop acquisition. */
esp_err_t ems_acquisition_stop(ems_acquisition_handle_t handle);

/* "ONLINE", "OFFLINE", "DISABLED" (never NULL). */
const char *ems_acquisition_mode_name(uint8_t mode);

/* Text for a Modbus result status (never NULL). */
const char *ems_acquisition_status_name(modbus_result_status_t status);

#ifdef __cplusplus
}
#endif
