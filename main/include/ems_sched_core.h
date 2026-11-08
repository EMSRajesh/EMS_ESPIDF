/*
 * MyEMS poll scheduler core (DP-WP4 4E, milestone D5).
 *
 * Pure C: no ESP-IDF or FreeRTOS dependency, so all scheduling decisions are
 * unit-tested on the host (test/host/test_ems_sched_core.c). The FreeRTOS task
 * in ems_poll_scheduler.c owns one instance and is its only caller.
 *
 * Design references (DP-WP4 Design Specification v1.1):
 *   9.1  poll entries = (device slot, poll group); request ID encoding
 *   9.2  deadlines, skip missed periods, one in-flight per device,
 *        earliest deadline first, rejected submissions, probe mode
 *   9.5  guard expiry never releases a slot
 *   9.6  worst-case transaction time T_wc
 *   9.7  bus hold on BUS_FAULT (D8): nothing is submitted, every device is
 *        in probe mode; on release each OFFLINE device is probed at once
 */
#ifndef EMS_SCHED_CORE_H
#define EMS_SCHED_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ems_config_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMS_SCHED_MAX_ENTRIES   EMS_CFG_MAX_POLL_ENTRIES   /* 16 */
#define EMS_SCHED_MAX_DEVICES   EMS_CFG_MAX_DEVICES        /* 4  */
#define EMS_SCHED_NO_ENTRY      0xFFu

/* Master timing constants (components/modbus_rtu_master/src/modbus_rtu_master.c),
 * also used by tools/profile/myems_profile_tool.py budget. */
#define EMS_SCHED_LATE_GUARD_MS        1000u
#define EMS_SCHED_RETRY_QUIET_MS       100u
#define EMS_SCHED_GUARD_MARGIN_MS      5000u
#define EMS_SCHED_SUBMIT_RETRY_MS      250u

/* ---- request IDs (Design 9.1) ------------------------------------------- */
#define EMS_SCHED_SEQ_MASK  0x00FFFFFFu

static inline uint32_t ems_sched_make_request_id(uint8_t entry, uint32_t seq24)
{
    return ((uint32_t)entry << 24) | (seq24 & EMS_SCHED_SEQ_MASK);
}

static inline uint8_t ems_sched_request_entry(uint32_t request_id)
{
    return (uint8_t)(request_id >> 24);
}

/* ---- immutable plan, built once from the generated configuration ------- */
typedef struct {
    uint32_t interval_ms;      /* group interval                               */
    uint32_t offset_ms;        /* first poll after start                        */
    uint32_t stale_timeout_ms;
    uint32_t timeout_ms;       /* per transaction (device comm setting)         */
    uint16_t start_address;
    uint16_t count;
    uint8_t  slot;             /* device slot                                   */
    uint8_t  group_index;      /* index in the device's map                     */
    uint8_t  slave_id;
    uint8_t  function_code;
    uint8_t  max_retries;
    bool     is_probe;         /* this group is the device's probe group        */
} ems_sched_entry_t;

typedef struct {
    int64_t  t_wc_us;          /* worst-case failed transaction (Design 9.6)    */
    int64_t  guard_us;         /* t_wc + 5 s                                    */
    uint32_t probe_interval_ms;
    uint8_t  first_entry;      /* entries of a device are contiguous            */
    uint8_t  entry_count;      /* 0 for a disabled device                       */
    uint8_t  probe_entry;      /* EMS_SCHED_NO_ENTRY if disabled                */
    bool     enabled;
} ems_sched_device_plan_t;

typedef struct {
    ems_sched_entry_t       entries[EMS_SCHED_MAX_ENTRIES];
    ems_sched_device_plan_t devices[EMS_SCHED_MAX_DEVICES];
    uint8_t entry_count;
    uint8_t device_count;
} ems_sched_plan_t;

typedef enum {
    EMS_SCHED_PLAN_OK = 0,
    EMS_SCHED_PLAN_NULL,
    EMS_SCHED_PLAN_TOO_MANY_DEVICES,
    EMS_SCHED_PLAN_TOO_MANY_ENTRIES,
    EMS_SCHED_PLAN_BAD_REFERENCE,   /* profile/map/bus/probe index out of range */
    EMS_SCHED_PLAN_BAD_GROUP,       /* FC not 3/4, count 0..125, interval 0     */
    EMS_SCHED_PLAN_BAD_DEVICE       /* slave id, timeout or retries out of range */
} ems_sched_plan_status_t;

/* Build the plan from the generated tables. Entries are ordered by device slot,
 * then by group index, so the same configuration always gives the same entry
 * numbers (acquisition relies on this to route results). Disabled devices get
 * no entries. */
ems_sched_plan_status_t ems_sched_plan_build(const ems_config_build_t *cfg, ems_sched_plan_t *plan);

const char *ems_sched_plan_status_name(ems_sched_plan_status_t status);

/* Worst-case failed transaction time in microseconds (Design 9.6). */
int64_t ems_sched_t_wc_us(uint32_t baud, uint8_t parity, uint8_t stop_bits,
                          uint32_t timeout_ms, uint8_t max_retries);

/* ---- dynamic state ------------------------------------------------------ */
typedef enum {
    EMS_SCHED_DEV_ONLINE = 0,
    EMS_SCHED_DEV_OFFLINE,      /* probe mode: only the probe group is polled  */
    EMS_SCHED_DEV_DISABLED
} ems_sched_dev_mode_t;

#define EMS_SCHED_HEALTH_FEEDBACK   0x01u   /* HEALTH_SCHED_FEEDBACK (Design 9.5) */
#define EMS_SCHED_HEALTH_BUS_HOLD   0x02u   /* polling held: HEALTH_BUS_FAULT (Design 9.7) */

typedef struct {
    uint32_t submitted;
    uint32_t skipped;           /* periods skipped, not caught up (device busy) */
    uint32_t rejected;          /* submission refused by the master             */
    uint32_t last_latency_ms;   /* submit -> feedback of the last request       */
    uint32_t max_latency_ms;
} ems_sched_entry_stats_t;

typedef struct {
    int64_t  next_due_us;
    ems_sched_entry_stats_t stats;
} ems_sched_entry_state_t;

typedef struct {
    int64_t  guard_deadline_us;
    int64_t  submitted_us;
    uint32_t pending_id;        /* 0 = idle                                     */
    uint8_t  pending_entry;
    uint8_t  mode;              /* ems_sched_dev_mode_t                          */
    bool     guard_expired;
} ems_sched_device_state_t;

typedef struct {
    uint32_t guard_expired;     /* sched_guard_expired                           */
    uint32_t feedback_matched;
    uint32_t feedback_unmatched;
    uint32_t feedback_late;     /* matched after the guard had expired           */
    uint32_t mode_changes;
    uint32_t bus_holds;         /* times polling was held for a bus fault (D8)  */
} ems_sched_stats_t;

typedef struct {
    ems_sched_plan_t          plan;
    ems_sched_entry_state_t   entry[EMS_SCHED_MAX_ENTRIES];
    ems_sched_device_state_t  device[EMS_SCHED_MAX_DEVICES];
    ems_sched_stats_t         stats;
    uint32_t                  seq24;            /* last used, 1..0xFFFFFF   */
    uint32_t                  submit_retry_ms;
    uint32_t                  health_flags;
    bool                      bus_hold;         /* Design 9.7: nothing is submitted */
} ems_sched_core_t;

/* A request the caller must submit to the master now. */
typedef struct {
    uint32_t request_id;
    uint8_t  entry;
    uint8_t  slot;
    uint8_t  slave_id;
    uint8_t  function_code;
    uint16_t start_address;
    uint16_t count;
    uint32_t timeout_ms;
    uint8_t  max_retries;
} ems_sched_request_t;

typedef enum {
    EMS_SCHED_FB_MATCHED = 0,   /* slot released                               */
    EMS_SCHED_FB_LATE,          /* slot released after its guard had expired    */
    EMS_SCHED_FB_UNMATCHED      /* no pending request with this ID: ignored     */
} ems_sched_feedback_t;

/* Initialise from a built plan. First deadlines are now + offset_ms. */
void ems_sched_init(ems_sched_core_t *core, const ems_sched_plan_t *plan,
                    uint32_t submit_retry_ms, int64_t now_us);

/*
 * Pick the next request to submit (Design 9.2): among due entries of devices
 * with nothing in flight and in a mode that allows the entry, the one with the
 * earliest deadline (lowest entry index on ties). Marks the device busy and
 * returns true; the caller must then report the submission outcome with
 * ems_sched_submitted(). Returns false when nothing is due.
 */
bool ems_sched_next(ems_sched_core_t *core, int64_t now_us, ems_sched_request_t *out);

/* Outcome of the master submission for the request returned by ems_sched_next(). */
void ems_sched_submitted(ems_sched_core_t *core, const ems_sched_request_t *req,
                         bool accepted, int64_t now_us);

/* Feedback from acquisition: the request with this ID has a terminal result. */
ems_sched_feedback_t ems_sched_feedback(ems_sched_core_t *core, uint32_t request_id, int64_t now_us);

/* Device mode from acquisition (ONLINE/OFFLINE). OFFLINE: only the probe group,
 * first probe one probe interval later. ONLINE: all groups due immediately.
 * Returns false for an invalid slot/mode or a disabled device. */
bool ems_sched_set_device_mode(ems_sched_core_t *core, uint8_t slot, ems_sched_dev_mode_t mode,
                               int64_t now_us);

/*
 * Bus hold (Design 9.7 step 5, D8). hold = true (BUS_FAULT): nothing is
 * submitted any more, every enabled ONLINE device goes to OFFLINE (probe
 * mode), HEALTH_BUS_HOLD is raised. Requests already in flight keep their
 * slots: the caller cancels them in the master, and the CANCELLED results
 * release the slots through normal feedback (the guard rule is unchanged).
 * Returns the bitmask of device slots with a request in flight.
 * hold = false (BUS_OK): submissions resume and every OFFLINE device's probe
 * is due at once; the first answer brings the device ONLINE as usual.
 * Repeating the current hold state changes nothing (returns the in-flight mask).
 */
uint32_t ems_sched_set_bus_hold(ems_sched_core_t *core, bool hold, int64_t now_us);

/* Number of devices with a request in flight. */
uint8_t ems_sched_in_flight(const ems_sched_core_t *core);

/*
 * Guard check (Design 9.5). For each busy device whose guard deadline has
 * passed: count, set the health flag, mark it, but DO NOT release the slot.
 * Returns the bitmask of device slots whose guard expired in this call
 * (each expiry is reported once).
 */
uint32_t ems_sched_check_guards(ems_sched_core_t *core, int64_t now_us);

/* Earliest time at which something may become due (for the task's wait). */
int64_t ems_sched_next_event_us(const ems_sched_core_t *core);

#ifdef __cplusplus
}
#endif

#endif /* EMS_SCHED_CORE_H */
