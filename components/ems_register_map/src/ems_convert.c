/*
 * ems_convert.c - scale/offset, CT/PT ratio, range check, map validation
 * (DP-WP4 4C, Design 7.3 steps 3-5)
 */
#include "ems_register_map.h"

#include <math.h>

/*
 * raw x scale, computed so that decimal scales stay exact where possible.
 * 0.1 and 0.01 are not representable in binary, so 2300 * 0.1 gives
 * 230.00000000000003. When 1/scale is a whole number (10, 100, 1000 ...)
 * divide by it instead: 2300 / 10 = 230 exactly, 10025 / 100 = 100.25.
 */
static double apply_scale(double raw, double scale)
{
    if (fabs(scale) < 1.0) {
        const double inv = 1.0 / scale;
        const double whole = round(inv);
        if (fabs(whole) >= 2.0 && fabs(inv - whole) <= 1e-9 * fabs(whole)) {
            return raw / whole;
        }
    }
    return raw * scale;
}

double ems_regmap_ratio_factor(const ems_reg_mapping_t *map, const ems_ratio_t *ratio)
{
    double factor = 1.0;
    if (map == NULL || ratio == NULL) {
        return factor;
    }
    if ((map->flags & EMS_MAP_F_APPLY_CT) != 0u) {
        factor *= ratio->ct;
    }
    if ((map->flags & EMS_MAP_F_APPLY_PT) != 0u) {
        factor *= ratio->pt;
    }
    return factor;
}

ems_decode_status_t ems_regmap_convert(const ems_reg_mapping_t *map,
                                       const ems_poll_group_t *group,
                                       const uint16_t *regs, uint16_t reg_count,
                                       const ems_ratio_t *ratio,
                                       ems_regmap_result_t *out)
{
    /* Steps 1-2: extract and decode. */
    const ems_decode_status_t st = ems_regmap_decode(map, group, regs, reg_count, out);
    if (st != EMS_DEC_OK) {
        return st;
    }

    /* Step 3: scale and offset. Step 4: ratio (the only place it is applied). */
    double value = apply_scale(out->value, map->scale) + map->offset;
    value *= ems_regmap_ratio_factor(map, ratio);

    if (!isfinite(value)) {
        out->value = 0.0;
        return EMS_DEC_INVALID;
    }
    out->value = value;

    /* Step 5: range check (inclusive limits). */
    if (value < map->min || value > map->max) {
        return EMS_DEC_OUT_OF_RANGE;
    }
    return EMS_DEC_OK;
}

/* ---------------------------------------------------------- validation */

static bool finite_d(double v)
{
    return isfinite(v) != 0;
}

ems_map_check_t ems_regmap_validate(const ems_poll_group_t *groups, size_t group_count,
                                    const ems_reg_mapping_t *maps, size_t map_count,
                                    size_t *bad_index)
{
    size_t dummy;
    size_t *bad = (bad_index != NULL) ? bad_index : &dummy;
    *bad = 0u;

    if (groups == NULL || group_count == 0u || (map_count > 0u && maps == NULL)) {
        return EMS_MAP_ERR_NULL;
    }

    for (size_t g = 0u; g < group_count; ++g) {
        const ems_poll_group_t *gr = &groups[g];
        *bad = g;
        if (gr->function_code != 3u && gr->function_code != 4u) {
            return EMS_MAP_ERR_GROUP_FC;
        }
        if (gr->count == 0u || gr->count > EMS_REGMAP_MAX_GROUP_REGS ||
            (uint32_t)gr->start_address + gr->count > 65536u) {
            return EMS_MAP_ERR_GROUP_COUNT;
        }
        if (gr->interval_ms < 1000u || gr->stale_timeout_ms <= gr->interval_ms) {
            return EMS_MAP_ERR_GROUP_TIMING;
        }
    }

    for (size_t i = 0u; i < map_count; ++i) {
        const ems_reg_mapping_t *m = &maps[i];
        *bad = i;
        if (m->group_index >= group_count) {
            return EMS_MAP_ERR_GROUP_REF;
        }
        const uint8_t nregs = ems_regmap_type_regs(m->type);
        if (nregs == 0u) {
            return EMS_MAP_ERR_TYPE;
        }
        if (!ems_regmap_order_valid(m->type, m->order)) {
            return EMS_MAP_ERR_ORDER;
        }
        const ems_poll_group_t *gr = &groups[m->group_index];
        if (m->address < gr->start_address ||
            (uint32_t)m->address + nregs > (uint32_t)gr->start_address + gr->count) {
            return EMS_MAP_ERR_OUTSIDE_BLOCK;
        }
        if (!finite_d(m->scale) || m->scale == 0.0 || !finite_d(m->offset)) {
            return EMS_MAP_ERR_SCALE;
        }
        if (!finite_d(m->min) || !finite_d(m->max) || !(m->min < m->max)) {
            return EMS_MAP_ERR_RANGE;
        }
        if (m->bit_mask != 0u && m->type != (uint8_t)EMS_REG_BITFIELD16) {
            return EMS_MAP_ERR_MASK;
        }
        if ((m->flags & EMS_MAP_F_ROLLOVER) != 0u &&
            ((m->flags & EMS_MAP_F_COUNTER) == 0u || m->type != (uint8_t)EMS_REG_UINT32)) {
            return EMS_MAP_ERR_FLAGS;      /* rollover only for UINT32 counters */
        }
        if ((m->flags & EMS_MAP_F_COUNTER) != 0u &&
            m->type != (uint8_t)EMS_REG_UINT32 && m->type != (uint8_t)EMS_REG_UINT16) {
            return EMS_MAP_ERR_FLAGS;      /* counters are unsigned integers */
        }
        for (size_t j = 0u; j < i; ++j) {
            if (maps[j].mapping_id == m->mapping_id) {
                return EMS_MAP_ERR_DUPLICATE_ID;
            }
        }
    }
    return EMS_MAP_OK;
}

const char *ems_map_check_name(ems_map_check_t check)
{
    switch (check) {
    case EMS_MAP_OK:                 return "OK";
    case EMS_MAP_ERR_NULL:           return "ERR_NULL";
    case EMS_MAP_ERR_GROUP_FC:       return "ERR_GROUP_FC";
    case EMS_MAP_ERR_GROUP_COUNT:    return "ERR_GROUP_COUNT";
    case EMS_MAP_ERR_GROUP_TIMING:   return "ERR_GROUP_TIMING";
    case EMS_MAP_ERR_GROUP_REF:      return "ERR_GROUP_REF";
    case EMS_MAP_ERR_TYPE:           return "ERR_TYPE";
    case EMS_MAP_ERR_ORDER:          return "ERR_ORDER";
    case EMS_MAP_ERR_OUTSIDE_BLOCK:  return "ERR_OUTSIDE_BLOCK";
    case EMS_MAP_ERR_SCALE:          return "ERR_SCALE";
    case EMS_MAP_ERR_RANGE:          return "ERR_RANGE";
    case EMS_MAP_ERR_MASK:           return "ERR_MASK";
    case EMS_MAP_ERR_FLAGS:          return "ERR_FLAGS";
    case EMS_MAP_ERR_DUPLICATE_ID:   return "ERR_DUPLICATE_ID";
    default:                         return "ERR_UNKNOWN";
    }
}
