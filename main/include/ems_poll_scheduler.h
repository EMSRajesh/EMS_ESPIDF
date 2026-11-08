#pragma once

/*
 * MyEMS poll scheduler task (DP-WP4 4E, milestone D5).
 *
 * FreeRTOS wrapper around the pure-C scheduler core (ems_sched_core.h). The
 * task is the only owner of the scheduling state. Other tasks talk to it only
 * through its message queue: completion feedback (acquisition), device
 * mode changes ONLINE/OFFLINE (acquisition, Design 9.4) and the bus hold on
 * BUS_FAULT / release on BUS_OK (acquisition, Design 9.7, D8).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "modbus_rtu_master.h"
#include "ems_sched_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ems_poll_scheduler *ems_poll_scheduler_handle_t;

typedef struct {
    modbus_master_handle_t master;
    const ems_sched_plan_t *plan;   /* built with ems_sched_plan_build(); copied */
    UBaseType_t task_priority;      /* 0 = 5                                    */
    uint32_t task_stack_size;       /* bytes, 0 = 4096                           */
    uint16_t feedback_queue_length; /* 0 = 16                                    */
    uint32_t submit_retry_delay_ms; /* 0 = 250                                   */
} ems_poll_scheduler_config_t;

typedef struct {
    ems_sched_stats_t       core;
    ems_sched_entry_stats_t entry[EMS_SCHED_MAX_ENTRIES];
    uint8_t                 device_mode[EMS_SCHED_MAX_DEVICES];
    uint8_t                 entry_count;
    uint32_t                health_flags;     /* EMS_SCHED_HEALTH_*              */
    uint32_t                bus_cancelled;    /* requests cancelled in the master on bus holds */
    uint32_t                bus_cancel_errors;/* cancel_pending calls that failed            */
    uint8_t                 in_flight;        /* devices with a request in flight            */
    bool                    bus_hold;         /* polling held (Design 9.7)                    */
    uint32_t                queue_full;       /* messages refused: queue full    */
    uint32_t                queue_hwm;        /* most messages waiting at once   */
} ems_poll_scheduler_stats_t;

esp_err_t ems_poll_scheduler_start(const ems_poll_scheduler_config_t *config,
                                   ems_poll_scheduler_handle_t *out_handle);

/* Acquisition: a terminal result for this request ID was consumed.
 * Non-blocking: ESP_ERR_TIMEOUT if the queue is full (counted). */
esp_err_t ems_poll_scheduler_notify_completion(ems_poll_scheduler_handle_t handle,
                                               uint32_t request_id);

/* Acquisition: device communication state changed (Design 9.4).
 * Non-blocking: ESP_ERR_TIMEOUT if the queue is full (counted). */
esp_err_t ems_poll_scheduler_set_device_mode(ems_poll_scheduler_handle_t handle,
                                             uint8_t slot, ems_sched_dev_mode_t mode);

/* Acquisition, Design 9.7 (D8). hold = true on BUS_FAULT: the scheduler stops
 * submitting, puts every device in probe mode and cancels the requests still
 * in the master (modbus_master_cancel_pending), whose CANCELLED results then
 * release the slots through normal feedback. hold = false on BUS_OK: polling
 * resumes and every OFFLINE device is probed at once.
 * Non-blocking: ESP_ERR_TIMEOUT if the queue is full (counted). */
esp_err_t ems_poll_scheduler_set_bus_hold(ems_poll_scheduler_handle_t handle, bool hold);

/* Copy of the latest counters (updated by the task on every loop). */
esp_err_t ems_poll_scheduler_get_stats(ems_poll_scheduler_handle_t handle,
                                       ems_poll_scheduler_stats_t *out);

/* Stops the scheduler task. Stop acquisition afterwards. */
esp_err_t ems_poll_scheduler_stop(ems_poll_scheduler_handle_t handle);

#ifdef __cplusplus
}
#endif
