/*
 * MyEMS acquisition core (DP-WP4 4E, milestone D6).
 *
 * Pure C (no ESP-IDF / FreeRTOS): everything acquisition decides about a
 * terminal Modbus result, so it is unit-tested on the host
 * (test/host/test_ems_acq_core.c). The acquisition task in ems_acquisition.c
 * is the only caller and the only writer of the state store.
 *
 * Design references (DP-WP4 Design Specification v1.1):
 *   7.3  conversion pipeline (via ems_register_map)
 *   8.5  energy counters (via ems_counter)
 *   9.1  routing by request ID; mismatches counted and discarded
 *   9.3  result -> quality table, freshness check
 *   9.4  consecutive comm failure counter (device state itself: D7)
 */
#ifndef EMS_ACQ_CORE_H
#define EMS_ACQ_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "ems_config_types.h"
#include "ems_counter.h"
#include "ems_measurement.h"
#include "ems_register_map.h"
#include "ems_sched_core.h"
#include "ems_state_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMS_ACQ_MAX_COUNTERS_PER_DEVICE 8u
#define EMS_ACQ_NO_COUNTER              0xFFu

/* Terminal result status, same meaning and order as modbus_result_status_t. */
typedef enum {
    EMS_ACQ_RES_OK = 0,
    EMS_ACQ_RES_TIMEOUT,
    EMS_ACQ_RES_CRC_ERROR,
    EMS_ACQ_RES_PROTOCOL_ERROR,
    EMS_ACQ_RES_EXCEPTION,
    EMS_ACQ_RES_TRANSPORT_ERROR,
    EMS_ACQ_RES_CANCELLED
} ems_acq_result_status_t;

/* The fields of a Modbus result that acquisition uses. */
typedef struct {
    const uint16_t *registers;
    uint32_t request_id;
    uint16_t register_count;
    uint8_t  slave_addr;
    uint8_t  function_code;
    uint8_t  status;            /* ems_acq_result_status_t */
    uint8_t  exception_code;
} ems_acq_result_t;

typedef enum {
    EMS_ACQ_OUT_ROUTING_ERROR = 0,  /* unknown entry or slave/FC mismatch: discarded  */
    EMS_ACQ_OUT_MISMATCH,           /* OK but wrong register count: group INVALID     */
    EMS_ACQ_OUT_OK,                 /* decoded and published                          */
    EMS_ACQ_OUT_EXCEPTION,          /* group INVALID, device reachable                */
    EMS_ACQ_OUT_FAILED,             /* timeout/CRC/protocol/transport: GOOD -> STALE  */
    EMS_ACQ_OUT_CANCELLED           /* nothing changed                                */
} ems_acq_outcome_kind_t;

/* What happened, for logging and events (filled by ems_acq_core_process). */
typedef struct {
    uint8_t  kind;              /* ems_acq_outcome_kind_t                     */
    uint8_t  entry;             /* EMS_SCHED_NO_ENTRY when unknown            */
    uint8_t  slot;
    uint8_t  group_index;
    uint8_t  good;              /* parameters published GOOD                  */
    uint8_t  out_of_range;
    uint8_t  invalid;
    uint8_t  stale;             /* parameters turned GOOD -> STALE            */
    uint8_t  exception_code;
    uint8_t  consecutive_failures;   /* of the device, after this result     */
    uint8_t  counter_discontinuities; /* in this result                       */
    uint8_t  counter_rollovers;
    uint16_t discontinuity_param;    /* last param with a discontinuity      */
} ems_acq_outcome_t;

/* Per-device counters (Design 9.9, the part acquisition owns). */
typedef struct {
    uint32_t ok;
    uint32_t timeouts;
    uint32_t crc_errors;
    uint32_t protocol_errors;
    uint32_t transport_errors;
    uint32_t exceptions;
    uint32_t cancelled;
    uint32_t mismatches;
    uint32_t counter_discontinuities;
    uint32_t counter_rollovers;
    int64_t  last_ok_us;
    uint8_t  last_exception;
    uint8_t  consecutive_failures;
} ems_acq_device_stats_t;

typedef struct {
    uint32_t results;
    uint32_t routing_errors;
} ems_acq_stats_t;

typedef struct {
    const ems_config_build_t *cfg;
    const ems_sched_plan_t   *plan;
    /* per device */
    const ems_cfg_map_t      *map[EMS_CFG_MAX_DEVICES];
    ems_ratio_t               ratio[EMS_CFG_MAX_DEVICES];
    bool                      apply_ratio[EMS_CFG_MAX_DEVICES];   /* MYEMS_APPLIES */
    double                    counter_max_rate[EMS_CFG_MAX_DEVICES];
    uint8_t                   counter_of[EMS_CFG_MAX_DEVICES][EMS_CFG_MAX_MAPPINGS_PER_MAP];
    ems_counter_t             counters[EMS_CFG_MAX_DEVICES][EMS_ACQ_MAX_COUNTERS_PER_DEVICE];
    double                    last_delta[EMS_CFG_MAX_DEVICES][EMS_ACQ_MAX_COUNTERS_PER_DEVICE];
    ems_acq_device_stats_t    dev[EMS_CFG_MAX_DEVICES];
    ems_acq_stats_t           stats;
    /* state store layout (must stay valid while the store is initialised) */
    ems_state_param_def_t     params[EMS_CFG_MAX_DEVICES][EMS_MAX_PARAMS_PER_DEVICE];
    ems_state_device_def_t    devices[EMS_CFG_MAX_DEVICES];
    ems_state_layout_t        layout;
} ems_acq_core_t;

typedef enum {
    EMS_ACQ_INIT_OK = 0,
    EMS_ACQ_INIT_NULL,
    EMS_ACQ_INIT_BAD_CONFIG,        /* reference out of range, too many params/counters */
    EMS_ACQ_INIT_STATE_STORE        /* ems_state_init failed                             */
} ems_acq_init_status_t;

/*
 * Build the state-store layout (one record per mapping; stale timeout of its
 * poll group; disabled devices NOT_SUPPORTED), initialise the store with the
 * given lock (NULL on the host) and set up counter trackers. The core, cfg
 * and plan must stay valid while the store is in use.
 */
ems_acq_init_status_t ems_acq_core_init(ems_acq_core_t *core, const ems_config_build_t *cfg,
                                        const ems_sched_plan_t *plan, const ems_state_lock_t *lock);

const char *ems_acq_init_status_name(ems_acq_init_status_t status);

/* Process one terminal result at time now_us (completion time). */
void ems_acq_core_process(ems_acq_core_t *core, const ems_acq_result_t *result, int64_t now_us,
                          ems_acq_outcome_t *outcome);

/* Freshness check (Design 9.3): GOOD older than its stale timeout -> STALE.
 * Returns the number of records changed. */
uint32_t ems_acq_core_tick(ems_acq_core_t *core, int64_t now_us);

/* Last counter delta of a counter parameter (0 if none). */
double ems_acq_core_counter_delta(const ems_acq_core_t *core, uint8_t slot, uint16_t param_id);

const char *ems_acq_outcome_name(ems_acq_outcome_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif /* EMS_ACQ_CORE_H */
