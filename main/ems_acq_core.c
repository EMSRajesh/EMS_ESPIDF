/*
 * MyEMS acquisition core (DP-WP4 4E, milestone D6). Pure C, see header.
 */
#include "ems_acq_core.h"

#include <string.h>

/* ------------------------------------------------------------------ init --- */

ems_acq_init_status_t ems_acq_core_init(ems_acq_core_t *core, const ems_config_build_t *cfg,
                                        const ems_sched_plan_t *plan, const ems_state_lock_t *lock)
{
    if (core == NULL || cfg == NULL || plan == NULL) {
        return EMS_ACQ_INIT_NULL;
    }
    memset(core, 0, sizeof(*core));
    core->cfg = cfg;
    core->plan = plan;
    if (cfg->device_count == 0u || cfg->device_count > EMS_CFG_MAX_DEVICES ||
        cfg->device_count > EMS_MAX_DEVICES) {
        return EMS_ACQ_INIT_BAD_CONFIG;
    }

    for (uint8_t s = 0u; s < cfg->device_count; s++) {
        const ems_cfg_device_t *d = &cfg->devices[s];
        if (d->profile_index >= cfg->profile_count) {
            return EMS_ACQ_INIT_BAD_CONFIG;
        }
        const ems_cfg_profile_t *p = &cfg->profiles[d->profile_index];
        if (p->map_index >= cfg->map_count) {
            return EMS_ACQ_INIT_BAD_CONFIG;
        }
        const ems_cfg_map_t *m = &cfg->maps[p->map_index];
        if (m->mapping_count > EMS_MAX_PARAMS_PER_DEVICE ||
            m->mapping_count > EMS_CFG_MAX_MAPPINGS_PER_MAP) {
            return EMS_ACQ_INIT_BAD_CONFIG;
        }
        core->map[s] = m;
        core->ratio[s].ct = d->ct_ratio;
        core->ratio[s].pt = d->pt_ratio;
        core->apply_ratio[s] = (p->ratio_handling == EMS_RATIO_MYEMS_APPLIES);
        core->counter_max_rate[s] = p->counter_max_rate_per_hour;

        uint8_t nc = 0u;
        for (uint16_t k = 0u; k < m->mapping_count; k++) {
            const ems_reg_mapping_t *mp = &m->mappings[k];
            if (mp->group_index >= m->group_count) {
                return EMS_ACQ_INIT_BAD_CONFIG;
            }
            core->params[s][k].param_id = mp->param_id;
            core->params[s][k].stale_timeout_ms = m->groups[mp->group_index].stale_timeout_ms;
            core->counter_of[s][k] = EMS_ACQ_NO_COUNTER;
            if ((mp->flags & EMS_MAP_F_COUNTER) != 0u) {
                if (nc >= EMS_ACQ_MAX_COUNTERS_PER_DEVICE) {
                    return EMS_ACQ_INIT_BAD_CONFIG;
                }
                const double factor = core->apply_ratio[s] ? ems_regmap_ratio_factor(mp, &core->ratio[s]) : 1.0;
                ems_counter_init(&core->counters[s][nc], (mp->flags & EMS_MAP_F_ROLLOVER) != 0u,
                                 mp->scale * factor);
                core->counter_of[s][k] = nc;
                nc++;
            }
        }
        core->devices[s].params = core->params[s];
        core->devices[s].param_count = (uint8_t)m->mapping_count;
        core->devices[s].enabled = d->enabled;
    }
    core->layout.devices = core->devices;
    core->layout.device_count = cfg->device_count;

    if (ems_state_is_initialised()) {
        ems_state_deinit();
    }
    if (ems_state_init(&core->layout, lock) != EMS_OK) {
        return EMS_ACQ_INIT_STATE_STORE;
    }
    return EMS_ACQ_INIT_OK;
}

const char *ems_acq_init_status_name(ems_acq_init_status_t status)
{
    switch (status) {
    case EMS_ACQ_INIT_OK:          return "OK";
    case EMS_ACQ_INIT_NULL:        return "NULL";
    case EMS_ACQ_INIT_BAD_CONFIG:  return "BAD_CONFIG";
    case EMS_ACQ_INIT_STATE_STORE: return "STATE_STORE";
    default:                       return "UNKNOWN";
    }
}

const char *ems_acq_outcome_name(ems_acq_outcome_kind_t kind)
{
    switch (kind) {
    case EMS_ACQ_OUT_ROUTING_ERROR: return "ROUTING_ERROR";
    case EMS_ACQ_OUT_MISMATCH:      return "RESPONSE_MISMATCH";
    case EMS_ACQ_OUT_OK:            return "OK";
    case EMS_ACQ_OUT_EXCEPTION:     return "EXCEPTION";
    case EMS_ACQ_OUT_FAILED:        return "FAILED";
    case EMS_ACQ_OUT_CANCELLED:     return "CANCELLED";
    default:                        return "UNKNOWN";
    }
}

/* -------------------------------------------------------------- helpers --- */

static void mark_group(ems_acq_core_t *core, uint8_t slot, uint8_t group, bool invalid,
                       ems_acq_outcome_t *o)
{
    const ems_cfg_map_t *m = core->map[slot];
    for (uint16_t k = 0u; k < m->mapping_count; k++) {
        const ems_reg_mapping_t *mp = &m->mappings[k];
        if (mp->group_index != group) {
            continue;
        }
        if (invalid) {
            (void)ems_state_mark_invalid(slot, mp->param_id);
            o->invalid++;
        } else {
            ems_measurement_record_t r;
            const bool was_good = ems_state_get(slot, mp->param_id, &r) == EMS_OK &&
                                  r.quality == (uint8_t)EMS_Q_GOOD;
            (void)ems_state_mark_failed(slot, mp->param_id);
            if (was_good) {
                o->stale++;
            }
        }
    }
}

static void decode_group(ems_acq_core_t *core, uint8_t slot, uint8_t group, const ems_poll_group_t *grp,
                         const uint16_t *regs, uint16_t count, int64_t now_us, ems_acq_outcome_t *o)
{
    const ems_cfg_map_t *m = core->map[slot];
    const ems_ratio_t *ratio = core->apply_ratio[slot] ? &core->ratio[slot] : NULL;
    for (uint16_t k = 0u; k < m->mapping_count; k++) {
        const ems_reg_mapping_t *mp = &m->mappings[k];
        if (mp->group_index != group) {
            continue;
        }
        ems_regmap_result_t res;
        const ems_decode_status_t st = ems_regmap_convert(mp, grp, regs, count, ratio, &res);
        if (st == EMS_DEC_OK) {
            (void)ems_state_publish(slot, mp->param_id, res.value, EMS_Q_GOOD, now_us);
            o->good++;
            const uint8_t c = core->counter_of[slot][k];
            if (c != EMS_ACQ_NO_COUNTER) {
                const double factor = (ratio != NULL) ? ems_regmap_ratio_factor(mp, ratio) : 1.0;
                double delta = 0.0;
                const ems_counter_event_t ev = ems_counter_update(
                    &core->counters[slot][c], (uint32_t)res.raw, res.value, now_us,
                    core->counter_max_rate[slot] * factor, &delta);
                core->last_delta[slot][c] = delta;
                if (ev == EMS_CNT_DISCONTINUITY) {
                    o->counter_discontinuities++;
                    o->discontinuity_param = mp->param_id;
                    core->dev[slot].counter_discontinuities++;
                } else if (ev == EMS_CNT_ROLLOVER) {
                    o->counter_rollovers++;
                    core->dev[slot].counter_rollovers++;
                }
            }
        } else if (st == EMS_DEC_OUT_OF_RANGE) {
            /* Value kept for diagnostics; no counter update (not GOOD). */
            (void)ems_state_publish(slot, mp->param_id, res.value, EMS_Q_OUT_OF_RANGE, now_us);
            o->out_of_range++;
        } else {
            (void)ems_state_mark_invalid(slot, mp->param_id);
            o->invalid++;
        }
    }
}

/* ------------------------------------------------------------- process --- */

void ems_acq_core_process(ems_acq_core_t *core, const ems_acq_result_t *r, int64_t now_us,
                          ems_acq_outcome_t *o)
{
    ems_acq_outcome_t dummy;
    if (o == NULL) {
        o = &dummy;
    }
    memset(o, 0, sizeof(*o));
    o->entry = EMS_SCHED_NO_ENTRY;
    o->kind = EMS_ACQ_OUT_ROUTING_ERROR;
    if (core == NULL || r == NULL) {
        return;
    }
    core->stats.results++;

    /* Route by request ID (Design 9.1). */
    const uint8_t e = ems_sched_request_entry(r->request_id);
    if (e >= core->plan->entry_count) {
        core->stats.routing_errors++;
        return;
    }
    const ems_sched_entry_t *en = &core->plan->entries[e];
    o->entry = e;
    o->slot = en->slot;
    o->group_index = en->group_index;
    if (r->slave_addr != en->slave_id || r->function_code != en->function_code ||
        en->slot >= core->cfg->device_count) {
        core->stats.routing_errors++;
        return;
    }

    const uint8_t slot = en->slot;
    ems_acq_device_stats_t *ds = &core->dev[slot];
    const ems_poll_group_t *grp = &core->map[slot]->groups[en->group_index];

    switch ((ems_acq_result_status_t)r->status) {
    case EMS_ACQ_RES_OK:
        ds->consecutive_failures = 0u;
        if (r->registers == NULL || !ems_regmap_response_complete(grp, r->register_count)) {
            /* Design 9.3 "OK, register count wrong" / DEC-009 / RTE-003:
             * counted as a routing error, nothing decoded, group INVALID. */
            core->stats.routing_errors++;
            ds->mismatches++;
            o->kind = EMS_ACQ_OUT_MISMATCH;
            mark_group(core, slot, en->group_index, true, o);
        } else {
            ds->ok++;
            ds->last_ok_us = now_us;
            o->kind = EMS_ACQ_OUT_OK;
            decode_group(core, slot, en->group_index, grp, r->registers, r->register_count, now_us, o);
        }
        break;

    case EMS_ACQ_RES_EXCEPTION:
        /* The device answered: reachable, but the data of this group is unusable. */
        ds->consecutive_failures = 0u;
        ds->exceptions++;
        ds->last_exception = r->exception_code;
        o->kind = EMS_ACQ_OUT_EXCEPTION;
        o->exception_code = r->exception_code;
        mark_group(core, slot, en->group_index, true, o);
        break;

    case EMS_ACQ_RES_TIMEOUT:
    case EMS_ACQ_RES_CRC_ERROR:
    case EMS_ACQ_RES_PROTOCOL_ERROR:
    case EMS_ACQ_RES_TRANSPORT_ERROR:
        if (ds->consecutive_failures < 0xFFu) {
            ds->consecutive_failures++;
        }
        if (r->status == EMS_ACQ_RES_TIMEOUT) {
            ds->timeouts++;
        } else if (r->status == EMS_ACQ_RES_CRC_ERROR) {
            ds->crc_errors++;
        } else if (r->status == EMS_ACQ_RES_PROTOCOL_ERROR) {
            ds->protocol_errors++;
        } else {
            ds->transport_errors++;
        }
        o->kind = EMS_ACQ_OUT_FAILED;
        mark_group(core, slot, en->group_index, false, o);
        break;

    case EMS_ACQ_RES_CANCELLED:
        ds->cancelled++;
        o->kind = EMS_ACQ_OUT_CANCELLED;
        break;

    default:
        core->stats.routing_errors++;
        o->kind = EMS_ACQ_OUT_ROUTING_ERROR;
        break;
    }
    o->consecutive_failures = ds->consecutive_failures;
}

uint32_t ems_acq_core_tick(ems_acq_core_t *core, int64_t now_us)
{
    (void)core;
    return ems_state_check_freshness(now_us);
}

double ems_acq_core_counter_delta(const ems_acq_core_t *core, uint8_t slot, uint16_t param_id)
{
    if (core == NULL || slot >= core->cfg->device_count || core->map[slot] == NULL) {
        return 0.0;
    }
    const ems_cfg_map_t *m = core->map[slot];
    for (uint16_t k = 0u; k < m->mapping_count; k++) {
        if (m->mappings[k].param_id == param_id && core->counter_of[slot][k] != EMS_ACQ_NO_COUNTER) {
            return core->last_delta[slot][core->counter_of[slot][k]];
        }
    }
    return 0.0;
}
