/*
 * ems_profile_activate.c - firmware activation checks (DP-WP4 4A). See ems_profile.h.
 */
#include "ems_profile.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "ems_measurement.h"

static const char *map_rule(ems_map_check_t c)
{
    switch (c) {
    case EMS_MAP_ERR_GROUP_FC:      return "V-07";
    case EMS_MAP_ERR_GROUP_COUNT:   return "V-07";
    case EMS_MAP_ERR_GROUP_TIMING:  return "V-09";
    case EMS_MAP_ERR_GROUP_REF:     return "V-06";
    case EMS_MAP_ERR_OUTSIDE_BLOCK: return "V-06";
    case EMS_MAP_ERR_TYPE:          return "V-05";
    case EMS_MAP_ERR_ORDER:         return "V-05";
    case EMS_MAP_ERR_MASK:          return "V-05";
    case EMS_MAP_ERR_FLAGS:         return "V-05";
    case EMS_MAP_ERR_SCALE:         return "V-08";
    case EMS_MAP_ERR_RANGE:         return "V-08";
    case EMS_MAP_ERR_DUPLICATE_ID:  return "V-02";
    case EMS_MAP_ERR_NULL:          return "V-01";
    case EMS_MAP_OK:
    default:                        return "V-??";
    }
}

static ems_activation_status_t fail(ems_activation_report_t *r, ems_activation_status_t st,
                                    const char *rule, uint8_t index, uint8_t sub)
{
    r->status = st;
    r->rule = rule;
    r->index = index;
    r->sub_index = sub;
    return st;
}

static bool finite_positive(double v)
{
    return isfinite(v) && v > 0.0;
}

ems_activation_status_t ems_profile_activate(const ems_config_build_t *b, bool sim_build,
                                             ems_activation_report_t *report)
{
    ems_activation_report_t dummy;
    ems_activation_report_t *r = (report != NULL) ? report : &dummy;
    memset(r, 0, sizeof(*r));
    r->rule = "OK";
    r->map_check = EMS_MAP_OK;
    r->sim_build = sim_build;

    if (b == NULL || b->buses == NULL || b->maps == NULL || b->profiles == NULL || b->devices == NULL) {
        return fail(r, EMS_ACT_NULL, "V-01", 0u, 0u);
    }

    /* V-15 and V-01 first: every count and pointer is checked before any
     * table is read, so the CRC computation below never reads out of bounds
     * even if a count in the generated file is corrupted. */
    if (b->bus_count == 0u || b->bus_count > EMS_CFG_MAX_BUSES || b->map_count == 0u ||
        b->map_count > EMS_CFG_MAX_MAPS || b->profile_count == 0u || b->profile_count > EMS_CFG_MAX_MAPS ||
        b->device_count == 0u || b->device_count > EMS_CFG_MAX_DEVICES) {
        return fail(r, EMS_ACT_LIMITS, "V-15", 0u, 0u);
    }
    for (uint8_t i = 0u; i < b->map_count; i++) {
        const ems_cfg_map_t *m = &b->maps[i];
        if (m->group_count == 0u || m->group_count > EMS_CFG_MAX_GROUPS_PER_MAP ||
            m->mapping_count > EMS_CFG_MAX_MAPPINGS_PER_MAP) {
            return fail(r, EMS_ACT_LIMITS, "V-15", i, 0u);
        }
        if (m->groups == NULL || (m->mapping_count > 0u && m->mappings == NULL)) {
            return fail(r, EMS_ACT_NULL, "V-01", i, 0u);
        }
    }

    /* Integrity: any edit to the tables after generation is caught here. */
    r->crc_stored = b->crc32;
    r->crc_computed = ems_config_crc32(b);
    if (r->crc_computed != r->crc_stored) {
        return fail(r, EMS_ACT_CRC, "CRC", 0u, 0u);
    }

    /* V-13: a simulation build runs only sim tables; a production build only PRODUCTION tables. */
    const uint8_t want = sim_build ? (uint8_t)EMS_TARGET_SIM : (uint8_t)EMS_TARGET_PROD;
    if (b->target != want || (!sim_build && b->classification != EMS_CLASS_PRODUCTION) ||
        b->classification > EMS_CLASS_TEST_ONLY) {
        return fail(r, EMS_ACT_TARGET, "V-13", 0u, 0u);
    }

    uint32_t total_mappings = 0u;
    for (uint8_t i = 0u; i < b->map_count; i++) {
        const ems_cfg_map_t *m = &b->maps[i];
        total_mappings += m->mapping_count;
        if (!sim_build && m->classification != EMS_CLASS_PRODUCTION) {
            return fail(r, EMS_ACT_TARGET, "V-13", i, 0u);
        }
        size_t bad = 0u;
        const ems_map_check_t c = ems_regmap_validate(m->groups, m->group_count, m->mappings,
                                                      m->mapping_count, &bad);
        if (c != EMS_MAP_OK) {
            r->map_check = c;
            return fail(r, EMS_ACT_MAP, map_rule(c), i, (uint8_t)bad);
        }
        /* V-04 (firmware part): every parameter known, at most once per map. */
        for (uint16_t k = 0u; k < m->mapping_count; k++) {
            if (ems_param_find(m->mappings[k].param_id) == NULL) {
                return fail(r, EMS_ACT_DICTIONARY, "V-04", i, (uint8_t)k);
            }
            for (uint16_t j = 0u; j < k; j++) {
                if (m->mappings[j].param_id == m->mappings[k].param_id) {
                    return fail(r, EMS_ACT_DICTIONARY, "V-02", i, (uint8_t)k);
                }
            }
        }
    }
    if (total_mappings > EMS_CFG_MAX_MAPPINGS) {
        return fail(r, EMS_ACT_LIMITS, "V-15", 0u, 0u);
    }

    for (uint8_t i = 0u; i < b->profile_count; i++) {
        const ems_cfg_profile_t *p = &b->profiles[i];
        if (p->map_index >= b->map_count) {
            return fail(r, EMS_ACT_REFERENCE, "V-03", i, 0u);
        }
        if (!sim_build && p->classification != EMS_CLASS_PRODUCTION) {
            return fail(r, EMS_ACT_TARGET, "V-13", i, 0u);
        }
        if (p->ratio_handling > EMS_RATIO_METER_REPORTS_PRIMARY) {
            return fail(r, EMS_ACT_RATIO, "V-12", i, 0u);
        }
    }

    for (uint8_t i = 0u; i < b->bus_count; i++) {
        const ems_cfg_bus_t *bus = &b->buses[i];
        if (bus->baud < 1200u || bus->baud > 115200u || bus->parity > EMS_PARITY_ODD ||
            (bus->stop_bits != 1u && bus->stop_bits != 2u)) {
            return fail(r, EMS_ACT_BUS, "V-10", i, 0u);
        }
    }

    uint16_t entries = 0u;
    for (uint8_t i = 0u; i < b->device_count; i++) {
        const ems_cfg_device_t *d = &b->devices[i];
        if (d->slot != i) {
            return fail(r, EMS_ACT_DEVICE, "V-02", i, 0u);
        }
        if (d->profile_index >= b->profile_count || d->bus_index >= b->bus_count) {
            return fail(r, EMS_ACT_REFERENCE, "V-03", i, 0u);
        }
        const ems_cfg_profile_t *p = &b->profiles[d->profile_index];
        const ems_cfg_map_t *m = &b->maps[p->map_index];
        if (d->slave_id < 1u || d->slave_id > 247u || d->timeout_ms < 50u || d->timeout_ms > 5000u ||
            d->max_retries > 5u || d->comm_lost_threshold < 1u || d->comm_lost_threshold > 10u ||
            d->probe_interval_ms < 1000u || d->probe_group >= m->group_count ||
            !finite_positive(d->ct_ratio) || !finite_positive(d->pt_ratio)) {
            return fail(r, EMS_ACT_DEVICE, "V-10", i, 0u);
        }
        for (uint8_t j = 0u; j < i; j++) {
            if (b->devices[j].bus_index == d->bus_index && b->devices[j].slave_id == d->slave_id) {
                return fail(r, EMS_ACT_DUPLICATE_SLAVE, "V-10", i, j);
            }
        }
        if (p->ratio_handling == EMS_RATIO_METER_REPORTS_PRIMARY && (d->ct_ratio != 1.0 || d->pt_ratio != 1.0)) {
            return fail(r, EMS_ACT_RATIO, "V-12", i, 0u);
        }
        if (d->enabled) {
            entries = (uint16_t)(entries + m->group_count);
        }
    }
    if (entries > EMS_CFG_MAX_POLL_ENTRIES) {
        return fail(r, EMS_ACT_LIMITS, "V-15", 0u, 0u);
    }
    return EMS_ACT_OK;
}

const char *ems_activation_status_name(ems_activation_status_t status)
{
    switch (status) {
    case EMS_ACT_OK:              return "OK";
    case EMS_ACT_NULL:            return "NULL";
    case EMS_ACT_CRC:             return "CRC";
    case EMS_ACT_TARGET:          return "TARGET";
    case EMS_ACT_LIMITS:          return "LIMITS";
    case EMS_ACT_REFERENCE:       return "REFERENCE";
    case EMS_ACT_MAP:             return "MAP";
    case EMS_ACT_DICTIONARY:      return "DICTIONARY";
    case EMS_ACT_BUS:             return "BUS";
    case EMS_ACT_DEVICE:          return "DEVICE";
    case EMS_ACT_DUPLICATE_SLAVE: return "DUPLICATE_SLAVE";
    case EMS_ACT_RATIO:           return "RATIO";
    default:                      return "UNKNOWN";
    }
}
