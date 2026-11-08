/*
 * MyEMS poll scheduler core (DP-WP4 4E, milestone D5). Pure C, see header.
 */
#include "ems_sched_core.h"

#include <string.h>

#define US_PER_MS 1000LL

static int64_t ms_to_us(uint32_t ms)
{
    return (int64_t)ms * US_PER_MS;
}

/* ------------------------------------------------------------------ plan --- */

int64_t ems_sched_t_wc_us(uint32_t baud, uint8_t parity, uint8_t stop_bits,
                          uint32_t timeout_ms, uint8_t max_retries)
{
    if (baud == 0u) {
        return 0;
    }
    /* start + 8 data + parity + stop bits per character; 8-byte read request */
    const int64_t char_bits = 1 + 8 + ((parity != EMS_PARITY_NONE) ? 1 : 0) + (int64_t)stop_bits;
    const int64_t t_req_us = (8 * char_bits * 1000000LL + (int64_t)baud - 1) / (int64_t)baud;
    const int64_t r = (int64_t)max_retries;
    return (r + 1) * (t_req_us + ms_to_us(timeout_ms) + ms_to_us(EMS_SCHED_LATE_GUARD_MS))
           + r * ms_to_us(EMS_SCHED_RETRY_QUIET_MS);
}

ems_sched_plan_status_t ems_sched_plan_build(const ems_config_build_t *cfg, ems_sched_plan_t *plan)
{
    if (cfg == NULL || plan == NULL || (cfg->device_count > 0u && cfg->devices == NULL)) {
        return EMS_SCHED_PLAN_NULL;
    }
    memset(plan, 0, sizeof(*plan));
    if (cfg->device_count > EMS_SCHED_MAX_DEVICES) {
        return EMS_SCHED_PLAN_TOO_MANY_DEVICES;
    }

    uint8_t n = 0u;
    for (uint8_t s = 0u; s < cfg->device_count; s++) {
        const ems_cfg_device_t *d = &cfg->devices[s];
        ems_sched_device_plan_t *dp = &plan->devices[s];
        dp->probe_entry = EMS_SCHED_NO_ENTRY;
        dp->first_entry = n;

        if (d->profile_index >= cfg->profile_count || d->bus_index >= cfg->bus_count) {
            return EMS_SCHED_PLAN_BAD_REFERENCE;
        }
        const ems_cfg_profile_t *p = &cfg->profiles[d->profile_index];
        if (p->map_index >= cfg->map_count) {
            return EMS_SCHED_PLAN_BAD_REFERENCE;
        }
        const ems_cfg_map_t *m = &cfg->maps[p->map_index];
        const ems_cfg_bus_t *b = &cfg->buses[d->bus_index];
        if (d->probe_group >= m->group_count || m->groups == NULL) {
            return EMS_SCHED_PLAN_BAD_REFERENCE;
        }
        if (d->slave_id < 1u || d->slave_id > 247u || d->timeout_ms < 50u || d->timeout_ms > 5000u ||
            d->max_retries > 5u || d->probe_interval_ms < 1000u) {
            return EMS_SCHED_PLAN_BAD_DEVICE;
        }

        dp->enabled = d->enabled;
        dp->probe_interval_ms = d->probe_interval_ms;
        dp->t_wc_us = ems_sched_t_wc_us(b->baud, b->parity, b->stop_bits, d->timeout_ms, d->max_retries);
        dp->guard_us = dp->t_wc_us + ms_to_us(EMS_SCHED_GUARD_MARGIN_MS);
        if (!d->enabled) {
            continue;
        }

        for (uint8_t g = 0u; g < m->group_count; g++) {
            const ems_poll_group_t *grp = &m->groups[g];
            if ((grp->function_code != 3u && grp->function_code != 4u) || grp->count == 0u ||
                grp->count > 125u || grp->interval_ms < 1000u ||
                (uint32_t)grp->start_address + grp->count > 65536u) {
                return EMS_SCHED_PLAN_BAD_GROUP;
            }
            if (n >= EMS_SCHED_MAX_ENTRIES) {
                return EMS_SCHED_PLAN_TOO_MANY_ENTRIES;
            }
            ems_sched_entry_t *e = &plan->entries[n];
            e->interval_ms = grp->interval_ms;
            e->offset_ms = grp->offset_ms;
            e->stale_timeout_ms = grp->stale_timeout_ms;
            e->timeout_ms = d->timeout_ms;
            e->start_address = grp->start_address;
            e->count = grp->count;
            e->slot = s;
            e->group_index = g;
            e->slave_id = d->slave_id;
            e->function_code = grp->function_code;
            e->max_retries = d->max_retries;
            e->is_probe = (g == d->probe_group);
            if (e->is_probe) {
                dp->probe_entry = n;
            }
            n++;
        }
        dp->entry_count = (uint8_t)(n - dp->first_entry);
    }
    plan->entry_count = n;
    plan->device_count = cfg->device_count;
    return EMS_SCHED_PLAN_OK;
}

const char *ems_sched_plan_status_name(ems_sched_plan_status_t status)
{
    switch (status) {
    case EMS_SCHED_PLAN_OK:               return "OK";
    case EMS_SCHED_PLAN_NULL:             return "NULL";
    case EMS_SCHED_PLAN_TOO_MANY_DEVICES: return "TOO_MANY_DEVICES";
    case EMS_SCHED_PLAN_TOO_MANY_ENTRIES: return "TOO_MANY_ENTRIES";
    case EMS_SCHED_PLAN_BAD_REFERENCE:    return "BAD_REFERENCE";
    case EMS_SCHED_PLAN_BAD_GROUP:        return "BAD_GROUP";
    case EMS_SCHED_PLAN_BAD_DEVICE:       return "BAD_DEVICE";
    default:                              return "UNKNOWN";
    }
}

/* ----------------------------------------------------------------- state --- */

static bool entry_allowed(const ems_sched_core_t *core, uint8_t e)
{
    const ems_sched_entry_t *en = &core->plan.entries[e];
    const uint8_t mode = core->device[en->slot].mode;
    if (mode == EMS_SCHED_DEV_ONLINE) {
        return true;
    }
    if (mode == EMS_SCHED_DEV_OFFLINE) {
        return en->is_probe;
    }
    return false;
}

static uint32_t entry_period_ms(const ems_sched_core_t *core, uint8_t e)
{
    const ems_sched_entry_t *en = &core->plan.entries[e];
    if (core->device[en->slot].mode == EMS_SCHED_DEV_OFFLINE && en->is_probe) {
        return core->plan.devices[en->slot].probe_interval_ms;
    }
    return en->interval_ms;
}

void ems_sched_init(ems_sched_core_t *core, const ems_sched_plan_t *plan,
                    uint32_t submit_retry_ms, int64_t now_us)
{
    if (core == NULL || plan == NULL) {
        return;
    }
    memset(core, 0, sizeof(*core));
    core->plan = *plan;
    core->submit_retry_ms = (submit_retry_ms != 0u) ? submit_retry_ms : EMS_SCHED_SUBMIT_RETRY_MS;
    for (uint8_t s = 0u; s < plan->device_count; s++) {
        core->device[s].mode = plan->devices[s].enabled ? (uint8_t)EMS_SCHED_DEV_ONLINE
                                                        : (uint8_t)EMS_SCHED_DEV_DISABLED;
        core->device[s].pending_entry = EMS_SCHED_NO_ENTRY;
    }
    for (uint8_t e = 0u; e < plan->entry_count; e++) {
        core->entry[e].next_due_us = now_us + ms_to_us(plan->entries[e].offset_ms);
    }
}

bool ems_sched_next(ems_sched_core_t *core, int64_t now_us, ems_sched_request_t *out)
{
    if (core == NULL || out == NULL) {
        return false;
    }
    uint8_t best = EMS_SCHED_NO_ENTRY;
    for (uint8_t e = 0u; e < core->plan.entry_count; e++) {
        if (!entry_allowed(core, e) || now_us < core->entry[e].next_due_us) {
            continue;
        }
        const uint8_t slot = core->plan.entries[e].slot;
        if (core->device[slot].pending_id != 0u) {
            continue;   /* rule 2: waits for the device's in-flight request */
        }
        if (best == EMS_SCHED_NO_ENTRY || core->entry[e].next_due_us < core->entry[best].next_due_us) {
            best = e;
        }
    }
    if (best == EMS_SCHED_NO_ENTRY) {
        return false;
    }

    core->seq24 = (core->seq24 >= EMS_SCHED_SEQ_MASK) ? 1u : core->seq24 + 1u;
    const ems_sched_entry_t *en = &core->plan.entries[best];
    out->request_id = ems_sched_make_request_id(best, core->seq24);
    out->entry = best;
    out->slot = en->slot;
    out->slave_id = en->slave_id;
    out->function_code = en->function_code;
    out->start_address = en->start_address;
    out->count = en->count;
    out->timeout_ms = en->timeout_ms;
    out->max_retries = en->max_retries;

    /* Mark busy before the caller submits, so a fast completion cannot race. */
    ems_sched_device_state_t *dev = &core->device[en->slot];
    dev->pending_id = out->request_id;
    dev->pending_entry = best;
    dev->submitted_us = now_us;
    dev->guard_expired = false;
    dev->guard_deadline_us = now_us + core->plan.devices[en->slot].guard_us;
    return true;
}

void ems_sched_submitted(ems_sched_core_t *core, const ems_sched_request_t *req,
                         bool accepted, int64_t now_us)
{
    if (core == NULL || req == NULL || req->entry >= core->plan.entry_count) {
        return;
    }
    ems_sched_entry_state_t *es = &core->entry[req->entry];
    ems_sched_device_state_t *dev = &core->device[req->slot];
    if (dev->pending_id != req->request_id) {
        return;
    }
    if (!accepted) {
        /* rule 4: retry after submit_retry_ms; the period is not consumed */
        es->stats.rejected++;
        dev->pending_id = 0u;
        dev->pending_entry = EMS_SCHED_NO_ENTRY;
        es->next_due_us = now_us + ms_to_us(core->submit_retry_ms);
        return;
    }

    es->stats.submitted++;
    /* rule 1: next deadline on the original grid, skipping missed periods
     * (no catch-up burst). The entry was due, so next_due <= now. */
    const int64_t period = ms_to_us(entry_period_ms(core, req->entry));
    const int64_t due = es->next_due_us;
    const int64_t missed = (now_us >= due) ? (now_us - due) / period : 0;
    const int64_t next = due + (missed + 1) * period;
    es->stats.skipped += (uint32_t)missed;
    es->next_due_us = next;
}

static void update_health(ems_sched_core_t *core)
{
    bool any = false;
    for (uint8_t s = 0u; s < core->plan.device_count; s++) {
        any = any || core->device[s].guard_expired;
    }
    if (any) {
        core->health_flags |= EMS_SCHED_HEALTH_FEEDBACK;
    } else {
        core->health_flags &= ~EMS_SCHED_HEALTH_FEEDBACK;
    }
}

ems_sched_feedback_t ems_sched_feedback(ems_sched_core_t *core, uint32_t request_id, int64_t now_us)
{
    if (core == NULL || request_id == 0u) {
        if (core != NULL) {
            core->stats.feedback_unmatched++;
        }
        return EMS_SCHED_FB_UNMATCHED;
    }
    for (uint8_t s = 0u; s < core->plan.device_count; s++) {
        ems_sched_device_state_t *dev = &core->device[s];
        if (dev->pending_id != 0u && dev->pending_id == request_id) {
            const bool late = dev->guard_expired;
            if (dev->pending_entry < core->plan.entry_count) {
                ems_sched_entry_stats_t *es = &core->entry[dev->pending_entry].stats;
                const int64_t lat = (now_us - dev->submitted_us) / US_PER_MS;
                es->last_latency_ms = (lat < 0) ? 0u : (uint32_t)lat;
                if (es->last_latency_ms > es->max_latency_ms) {
                    es->max_latency_ms = es->last_latency_ms;
                }
            }
            dev->pending_id = 0u;
            dev->pending_entry = EMS_SCHED_NO_ENTRY;
            dev->guard_expired = false;
            update_health(core);
            if (late) {
                core->stats.feedback_late++;
                return EMS_SCHED_FB_LATE;
            }
            core->stats.feedback_matched++;
            return EMS_SCHED_FB_MATCHED;
        }
    }
    core->stats.feedback_unmatched++;
    return EMS_SCHED_FB_UNMATCHED;
}

bool ems_sched_set_device_mode(ems_sched_core_t *core, uint8_t slot, ems_sched_dev_mode_t mode,
                               int64_t now_us)
{
    if (core == NULL || slot >= core->plan.device_count ||
        (mode != EMS_SCHED_DEV_ONLINE && mode != EMS_SCHED_DEV_OFFLINE)) {
        return false;
    }
    const ems_sched_device_plan_t *dp = &core->plan.devices[slot];
    ems_sched_device_state_t *dev = &core->device[slot];
    if (!dp->enabled || dev->mode == EMS_SCHED_DEV_DISABLED) {
        return false;
    }
    if (dev->mode == (uint8_t)mode) {
        return true;
    }
    dev->mode = (uint8_t)mode;
    core->stats.mode_changes++;
    if (mode == EMS_SCHED_DEV_OFFLINE) {
        /* probe mode: only the probe group, first probe one probe interval later */
        if (dp->probe_entry != EMS_SCHED_NO_ENTRY) {
            core->entry[dp->probe_entry].next_due_us = now_us + ms_to_us(dp->probe_interval_ms);
        }
    } else {
        /* back ONLINE: all groups due immediately, served one at a time */
        for (uint8_t e = dp->first_entry; e < dp->first_entry + dp->entry_count; e++) {
            core->entry[e].next_due_us = now_us;
        }
    }
    return true;
}

uint32_t ems_sched_check_guards(ems_sched_core_t *core, int64_t now_us)
{
    uint32_t expired = 0u;
    if (core == NULL) {
        return 0u;
    }
    for (uint8_t s = 0u; s < core->plan.device_count; s++) {
        ems_sched_device_state_t *dev = &core->device[s];
        if (dev->pending_id != 0u && !dev->guard_expired && now_us >= dev->guard_deadline_us) {
            /* Design 9.5: count and flag, but never release the slot. */
            dev->guard_expired = true;
            core->stats.guard_expired++;
            expired |= 1u << s;
        }
    }
    if (expired != 0u) {
        update_health(core);
    }
    return expired;
}

int64_t ems_sched_next_event_us(const ems_sched_core_t *core)
{
    int64_t t = INT64_MAX;
    if (core == NULL) {
        return t;
    }
    for (uint8_t e = 0u; e < core->plan.entry_count; e++) {
        const uint8_t slot = core->plan.entries[e].slot;
        if (entry_allowed(core, e) && core->device[slot].pending_id == 0u &&
            core->entry[e].next_due_us < t) {
            t = core->entry[e].next_due_us;
        }
    }
    for (uint8_t s = 0u; s < core->plan.device_count; s++) {
        const ems_sched_device_state_t *dev = &core->device[s];
        if (dev->pending_id != 0u && !dev->guard_expired && dev->guard_deadline_us < t) {
            t = dev->guard_deadline_us;
        }
    }
    return t;
}
