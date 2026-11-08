/*
 * ems_event.h - MyEMS event ring (DP-WP4 Design 9.9, milestone D7)
 *
 * A bounded RAM ring of the last EMS_EVENT_RING_SIZE events. Producers
 * (app_main, scheduler, acquisition) call ems_event_emit(); when the ring is
 * full the oldest event is overwritten and event_drops is incremented
 * (DIAG-003). Every event gets a sequence number (1, 2, 3, ...), so a later
 * consumer, for example persistent storage, can read new events with
 * ems_event_get() without changing the producers.
 *
 * Pure C, no RTOS dependency: the lock is injected (short critical section on
 * the target, NULL on the host). No dynamic memory. 16 bytes per event.
 */
#ifndef EMS_EVENT_H
#define EMS_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EMS_EVENT_RING_SIZE
#define EMS_EVENT_RING_SIZE 64u
#endif

typedef enum {
    EMS_EVT_NONE = 0,
    EMS_EVT_CONFIG_ACTIVATED,
    EMS_EVT_CONFIG_REJECTED,
    EMS_EVT_DEVICE_ONLINE,
    EMS_EVT_DEVICE_OFFLINE,
    EMS_EVT_MODBUS_EXCEPTION,
    EMS_EVT_RESPONSE_MISMATCH,
    EMS_EVT_COUNTER_DISCONTINUITY,
    EMS_EVT_COUNTER_ROLLOVER,
    EMS_EVT_SCHED_GUARD_EXPIRED,
    EMS_EVT_BUS_FAULT,
    EMS_EVT_BUS_OK,
    EMS_EVT_TYPE_COUNT
} ems_event_type_t;

#define EMS_EVENT_NO_SLOT  0xFFu
#define EMS_EVENT_NO_PARAM 0x0000u

typedef struct {
    int64_t  mono_us;      /* monotonic time of the event               */
    uint32_t data;         /* type-specific: exception code, CRC, ...   */
    uint16_t param_id;     /* EMS_EVENT_NO_PARAM if not applicable      */
    uint8_t  type;         /* ems_event_type_t                          */
    uint8_t  slot;         /* device slot, EMS_EVENT_NO_SLOT if none    */
} ems_event_t;             /* 16 bytes */

typedef void (*ems_event_lock_fn_t)(void *ctx);

typedef struct {
    ems_event_lock_fn_t lock;
    ems_event_lock_fn_t unlock;
    void *ctx;
} ems_event_lock_t;

typedef struct {
    uint32_t emitted;      /* total events emitted = last sequence number */
    uint32_t drops;        /* events overwritten because the ring was full */
    uint32_t stored;       /* events currently in the ring                */
} ems_event_stats_t;

/* Clear the ring and set the lock (NULL: no locking). */
void ems_event_init(const ems_event_lock_t *lock);

/* Record an event. Returns its sequence number (>= 1), 0 for an invalid type. */
uint32_t ems_event_emit(ems_event_type_t type, uint8_t slot, uint16_t param_id, uint32_t data,
                        int64_t now_mono_us);

/*
 * Copy the event with sequence number seq. Returns false if seq is 0, not yet
 * emitted, or already overwritten.
 */
bool ems_event_get(uint32_t seq, ems_event_t *out);

/* Oldest sequence number still in the ring (0 if the ring is empty). */
uint32_t ems_event_oldest_seq(void);

void ems_event_get_stats(ems_event_stats_t *out);

/* "DEVICE_OFFLINE", ... ; never NULL. */
const char *ems_event_type_name(ems_event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* EMS_EVENT_H */
