/*
 * MyEMS bus health (DP-WP4 milestone D8, Design 9.7). See ems_bus_health.h.
 */
#include "ems_bus_health.h"

#include <string.h>

#define US_PER_MS 1000LL

void ems_bus_health_init(ems_bus_health_t *h, const ems_bus_health_config_t *cfg, int64_t now_us)
{
    if (h == NULL) {
        return;
    }
    memset(h, 0, sizeof(*h));
    if (cfg != NULL) {
        h->cfg = *cfg;
    }
    if (h->cfg.recovery_limit_ms == 0u) {
        h->cfg.recovery_limit_ms = EMS_BUS_RECOVERY_LIMIT_MS;
    }
    h->state = (uint8_t)EMS_BUS_OK;
    h->last_transport = (uint8_t)EMS_BUS_TR_UNKNOWN;
    h->last_progress_us = now_us;
}

/* Master progress: it completed something, or holds nothing. */
static void track_progress(ems_bus_health_t *h, const ems_bus_inputs_t *in, int64_t now_us)
{
    if (!in->counters_valid) {
        return;
    }
    h->outstanding = in->accepted - in->completed;   /* modulo 2^32, like the counters */
    if (!h->progress_init || in->completed != h->last_completed || h->outstanding == 0u) {
        h->last_progress_us = now_us;
        h->last_completed = in->completed;
        h->progress_init = true;
    }
}

static void track_recovery(ems_bus_health_t *h, const ems_bus_inputs_t *in, int64_t now_us)
{
    if (in->transport == (uint8_t)EMS_BUS_TR_RECOVERING) {
        if (!h->recovering) {
            h->recovering = true;
            h->recovering_since_us = now_us;
        }
    } else if (in->transport != (uint8_t)EMS_BUS_TR_UNKNOWN) {
        h->recovering = false;
    }
}

static ems_bus_reason_t derived_fault(const ems_bus_health_t *h, const ems_bus_inputs_t *in, int64_t now_us)
{
    if (in->transport == (uint8_t)EMS_BUS_TR_FAILED) {
        return EMS_BUS_REASON_TRANSPORT_FAILED;
    }
    if (h->recovering &&
        now_us - h->recovering_since_us >= (int64_t)h->cfg.recovery_limit_ms * US_PER_MS) {
        return EMS_BUS_REASON_RECOVERY_STUCK;
    }
    if (h->cfg.stall_limit_ms != 0u && in->counters_valid && h->outstanding != 0u &&
        now_us - h->last_progress_us >= (int64_t)h->cfg.stall_limit_ms * US_PER_MS) {
        return EMS_BUS_REASON_MASTER_STALLED;
    }
    return EMS_BUS_REASON_NONE;
}

static ems_bus_change_t enter_fault(ems_bus_health_t *h, ems_bus_reason_t reason, int64_t now_us)
{
    h->state = (uint8_t)EMS_BUS_FAULT;
    h->reason = (uint8_t)reason;
    h->fault_since_us = now_us;
    h->faults++;
    return EMS_BUS_CHANGE_FAULT;
}

ems_bus_change_t ems_bus_health_update(ems_bus_health_t *h, const ems_bus_inputs_t *in, int64_t now_us)
{
    if (h == NULL || in == NULL) {
        return EMS_BUS_CHANGE_NONE;
    }
    h->last_transport = in->transport;
    track_progress(h, in, now_us);
    track_recovery(h, in, now_us);

    ems_bus_reason_t reason = derived_fault(h, in, now_us);
    const bool faulted = (h->state == (uint8_t)EMS_BUS_FAULT);

    if (in->master_report == (uint8_t)EMS_BUS_REPORT_NONE) {
        if (faulted) {
            return EMS_BUS_CHANGE_NONE;          /* latched: fail closed until the master reports */
        }
        if (reason != EMS_BUS_REASON_NONE) {
            return enter_fault(h, reason, now_us);
        }
        h->state = (uint8_t)(h->recovering ? EMS_BUS_RECOVERING : EMS_BUS_OK);
        return EMS_BUS_CHANGE_NONE;
    }

    /* The master reports its health (CR-D8-01): the report decides, derived faults still count. */
    if (reason == EMS_BUS_REASON_NONE && in->master_report == (uint8_t)EMS_BUS_REPORT_FAULT) {
        reason = EMS_BUS_REASON_MASTER_REPORT;
    }
    if (reason != EMS_BUS_REASON_NONE) {
        return faulted ? EMS_BUS_CHANGE_NONE : enter_fault(h, reason, now_us);
    }
    if (faulted && in->master_report != (uint8_t)EMS_BUS_REPORT_OK) {
        return EMS_BUS_CHANGE_NONE;              /* a recovery attempt is running: not proven OK yet */
    }
    const bool recovering = h->recovering || in->master_report == (uint8_t)EMS_BUS_REPORT_RECOVERING;
    h->state = (uint8_t)(recovering ? EMS_BUS_RECOVERING : EMS_BUS_OK);
    if (faulted) {
        h->recoveries++;
        return EMS_BUS_CHANGE_OK;
    }
    return EMS_BUS_CHANGE_NONE;
}

ems_bus_state_t ems_bus_health_state(const ems_bus_health_t *h)
{
    return (h == NULL) ? EMS_BUS_FAULT : (ems_bus_state_t)h->state;
}

const char *ems_bus_state_name(ems_bus_state_t state)
{
    switch (state) {
    case EMS_BUS_OK:         return "BUS_OK";
    case EMS_BUS_RECOVERING: return "BUS_RECOVERING";
    case EMS_BUS_FAULT:      return "BUS_FAULT";
    default:                 return "UNKNOWN";
    }
}

const char *ems_bus_reason_name(ems_bus_reason_t reason)
{
    switch (reason) {
    case EMS_BUS_REASON_NONE:             return "NONE";
    case EMS_BUS_REASON_TRANSPORT_FAILED: return "TRANSPORT_FAILED";
    case EMS_BUS_REASON_RECOVERY_STUCK:   return "RECOVERY_STUCK";
    case EMS_BUS_REASON_MASTER_STALLED:   return "MASTER_STALLED";
    case EMS_BUS_REASON_MASTER_REPORT:    return "MASTER_REPORT";
    default:                              return "UNKNOWN";
    }
}

const char *ems_bus_transport_name(ems_bus_transport_t transport)
{
    switch (transport) {
    case EMS_BUS_TR_IDLE:       return "IDLE";
    case EMS_BUS_TR_RECOVERING: return "RECOVERING";
    case EMS_BUS_TR_FAILED:     return "FAILED";
    case EMS_BUS_TR_UNKNOWN:    return "UNKNOWN";
    default:                    return "UNKNOWN";
    }
}
