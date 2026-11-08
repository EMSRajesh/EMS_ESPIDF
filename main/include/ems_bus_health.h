/*
 * MyEMS bus health (DP-WP4 milestone D8, Design 9.7).
 *
 * Pure C (no ESP-IDF / FreeRTOS): decides the health of one RS-485 bus from
 * what the COM layer exposes, so every rule is unit-tested on the host
 * (test/host/test_ems_bus_health.c). The acquisition task owns one instance
 * and is its only caller.
 *
 * Why derived: the 09_057 master has no bus-health API (Design 9.7 step 2)
 * and never leaves its fail-closed state after RS-485 recovery FAILED
 * (steps 1-3; see docs/D8_ESCALATION_REVIEW.md, COM change request CR-D8-01).
 * Until the master reports health itself, FAULT is derived from:
 *   - the transport recovery state is FAILED                 (TRANSPORT_FAILED)
 *   - the transport stays in recovery longer than the limit   (RECOVERY_STUCK)
 *   - the master holds requests but completes none for longer
 *     than the stall limit (the scheduler guard)              (MASTER_STALLED)
 * and FAULT is LATCHED (fail closed): with the current master the bus cannot
 * recover without a reboot, and declaring it OK would only feed requests to a
 * master that never answers them.
 *
 * When the master reports health (input master_report != NONE, after the CR),
 * the report decides OK / RECOVERING / FAULT, and FAULT is left only when the
 * report is OK (not while a recovery attempt reports RECOVERING) and no
 * derived condition holds.
 */
#ifndef EMS_BUS_HEALTH_H
#define EMS_BUS_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMS_BUS_RECOVERY_LIMIT_MS  5000u   /* a normal recovery takes <= ~1.1 s     */

typedef enum {
    EMS_BUS_OK = 0,
    EMS_BUS_RECOVERING,         /* transport recovery in progress (normal after a failed transaction) */
    EMS_BUS_FAULT               /* HEALTH_BUS_FAULT: polling held, devices OFFLINE                    */
} ems_bus_state_t;

/* Transport recovery state as seen by the application. */
typedef enum {
    EMS_BUS_TR_IDLE = 0,        /* RS485_RECOVERY_IDLE                                  */
    EMS_BUS_TR_RECOVERING,      /* RS485_RECOVERY_REQUESTED / ACTIVE / COMPLETE          */
    EMS_BUS_TR_FAILED,          /* RS485_RECOVERY_FAILED (terminal in 09_057)            */
    EMS_BUS_TR_UNKNOWN          /* state could not be read: no information               */
} ems_bus_transport_t;

/* Health reported by the master itself (Design 9.7 step 2). */
typedef enum {
    EMS_BUS_REPORT_NONE = 0,    /* master has no health API (09_057)                     */
    EMS_BUS_REPORT_OK,
    EMS_BUS_REPORT_RECOVERING,
    EMS_BUS_REPORT_FAULT
} ems_bus_report_t;

typedef enum {
    EMS_BUS_REASON_NONE = 0,
    EMS_BUS_REASON_TRANSPORT_FAILED,
    EMS_BUS_REASON_RECOVERY_STUCK,
    EMS_BUS_REASON_MASTER_STALLED,
    EMS_BUS_REASON_MASTER_REPORT
} ems_bus_reason_t;

typedef struct {
    uint8_t  transport;         /* ems_bus_transport_t                                   */
    uint8_t  master_report;     /* ems_bus_report_t                                      */
    bool     counters_valid;    /* accepted/completed were read                          */
    uint32_t accepted;          /* modbus_master_stats_t.requests_accepted               */
    uint32_t completed;         /* modbus_master_stats_t.requests_completed              */
} ems_bus_inputs_t;

typedef struct {
    uint32_t recovery_limit_ms; /* 0 = EMS_BUS_RECOVERY_LIMIT_MS                         */
    uint32_t stall_limit_ms;    /* largest scheduler guard (T_wc + 5 s); 0 = no stall check */
} ems_bus_health_config_t;

typedef struct {
    ems_bus_health_config_t cfg;
    uint8_t  state;             /* ems_bus_state_t                                       */
    uint8_t  reason;            /* ems_bus_reason_t of the current / last FAULT          */
    uint8_t  last_transport;    /* last input, for diagnostics                           */
    bool     progress_init;
    bool     recovering;            /* transport in recovery at the last update          */
    int64_t  recovering_since_us;
    int64_t  last_progress_us;      /* last time the master completed something or held nothing */
    int64_t  fault_since_us;
    uint32_t last_completed;
    uint32_t outstanding;       /* accepted - completed at the last update               */
    uint32_t faults;            /* entries into FAULT                                    */
    uint32_t recoveries;        /* FAULT -> OK                                           */
} ems_bus_health_t;

typedef enum {
    EMS_BUS_CHANGE_NONE = 0,
    EMS_BUS_CHANGE_FAULT,       /* entered FAULT now: EVT_BUS_FAULT, hold polling, devices OFFLINE */
    EMS_BUS_CHANGE_OK           /* left FAULT now:    EVT_BUS_OK, release, probe                    */
} ems_bus_change_t;

void ems_bus_health_init(ems_bus_health_t *h, const ems_bus_health_config_t *cfg, int64_t now_us);

/* Feed the current inputs (call periodically, e.g. every 100 ms). */
ems_bus_change_t ems_bus_health_update(ems_bus_health_t *h, const ems_bus_inputs_t *in, int64_t now_us);

ems_bus_state_t ems_bus_health_state(const ems_bus_health_t *h);

const char *ems_bus_state_name(ems_bus_state_t state);
const char *ems_bus_reason_name(ems_bus_reason_t reason);
const char *ems_bus_transport_name(ems_bus_transport_t transport);

#ifdef __cplusplus
}
#endif

#endif /* EMS_BUS_HEALTH_H */
