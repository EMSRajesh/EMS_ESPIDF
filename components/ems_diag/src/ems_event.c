/*
 * ems_event.c - MyEMS event ring (DP-WP4 Design 9.9). See ems_event.h.
 */
#include "ems_event.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(ems_event_t) == 16u, "ems_event_t must be 16 bytes (Design 11)");

static ems_event_t s_ring[EMS_EVENT_RING_SIZE];
static uint32_t s_emitted;
static ems_event_lock_t s_lock;

static void lock(void)
{
    if (s_lock.lock != NULL) {
        s_lock.lock(s_lock.ctx);
    }
}

static void unlock(void)
{
    if (s_lock.unlock != NULL) {
        s_lock.unlock(s_lock.ctx);
    }
}

void ems_event_init(const ems_event_lock_t *lock_cfg)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_emitted = 0u;
    if (lock_cfg != NULL) {
        s_lock = *lock_cfg;
    } else {
        memset(&s_lock, 0, sizeof(s_lock));
    }
}

uint32_t ems_event_emit(ems_event_type_t type, uint8_t slot, uint16_t param_id, uint32_t data,
                        int64_t now_mono_us)
{
    if (type <= EMS_EVT_NONE || type >= EMS_EVT_TYPE_COUNT) {
        return 0u;
    }
    lock();
    const uint32_t seq = s_emitted + 1u;
    ems_event_t *e = &s_ring[(seq - 1u) % EMS_EVENT_RING_SIZE];
    e->mono_us = now_mono_us;
    e->data = data;
    e->param_id = param_id;
    e->type = (uint8_t)type;
    e->slot = slot;
    s_emitted = seq;
    unlock();
    return seq;
}

static uint32_t oldest_locked(void)
{
    if (s_emitted == 0u) {
        return 0u;
    }
    return (s_emitted > EMS_EVENT_RING_SIZE) ? s_emitted - EMS_EVENT_RING_SIZE + 1u : 1u;
}

bool ems_event_get(uint32_t seq, ems_event_t *out)
{
    if (out == NULL || seq == 0u) {
        return false;
    }
    bool ok = false;
    lock();
    if (seq <= s_emitted && seq >= oldest_locked()) {
        *out = s_ring[(seq - 1u) % EMS_EVENT_RING_SIZE];
        ok = true;
    }
    unlock();
    return ok;
}

uint32_t ems_event_oldest_seq(void)
{
    lock();
    const uint32_t o = oldest_locked();
    unlock();
    return o;
}

void ems_event_get_stats(ems_event_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    lock();
    out->emitted = s_emitted;
    out->stored = (s_emitted > EMS_EVENT_RING_SIZE) ? EMS_EVENT_RING_SIZE : s_emitted;
    out->drops = (s_emitted > EMS_EVENT_RING_SIZE) ? s_emitted - EMS_EVENT_RING_SIZE : 0u;
    unlock();
}

const char *ems_event_type_name(ems_event_type_t type)
{
    switch (type) {
    case EMS_EVT_CONFIG_ACTIVATED:      return "CONFIG_ACTIVATED";
    case EMS_EVT_CONFIG_REJECTED:       return "CONFIG_REJECTED";
    case EMS_EVT_DEVICE_ONLINE:         return "DEVICE_ONLINE";
    case EMS_EVT_DEVICE_OFFLINE:        return "DEVICE_OFFLINE";
    case EMS_EVT_MODBUS_EXCEPTION:      return "MODBUS_EXCEPTION";
    case EMS_EVT_RESPONSE_MISMATCH:     return "RESPONSE_MISMATCH";
    case EMS_EVT_COUNTER_DISCONTINUITY: return "COUNTER_DISCONTINUITY";
    case EMS_EVT_COUNTER_ROLLOVER:      return "COUNTER_ROLLOVER";
    case EMS_EVT_SCHED_GUARD_EXPIRED:   return "SCHED_GUARD_EXPIRED";
    case EMS_EVT_BUS_FAULT:             return "BUS_FAULT";
    case EMS_EVT_BUS_OK:                return "BUS_OK";
    case EMS_EVT_NONE:
    case EMS_EVT_TYPE_COUNT:
    default:                            return "UNKNOWN";
    }
}
