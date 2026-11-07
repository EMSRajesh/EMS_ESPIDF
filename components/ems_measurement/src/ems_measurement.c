/*
 * ems_measurement.c - dictionary lookup and text helpers (DP-WP4 4B)
 */
#include "ems_measurement.h"

#include <string.h>

const ems_param_info_t *ems_param_find(uint16_t runtime_id)
{
    /* Table is sorted by runtime ID: binary search. */
    size_t lo = 0u;
    size_t hi = ems_param_table_len;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2u;
        const uint16_t id = ems_param_table[mid].runtime_id;
        if (id == runtime_id) {
            return &ems_param_table[mid];
        }
        if (id < runtime_id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

const ems_param_info_t *ems_param_find_by_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < ems_param_table_len; ++i) {
        if (strcmp(ems_param_table[i].name, name) == 0) {
            return &ems_param_table[i];
        }
    }
    return NULL;
}

const char *ems_quality_name(ems_quality_t quality)
{
    switch (quality) {
    case EMS_Q_INITIALIZING:  return "INITIALIZING";
    case EMS_Q_GOOD:          return "GOOD";
    case EMS_Q_STALE:         return "STALE";
    case EMS_Q_COMM_LOST:     return "COMM_LOST";
    case EMS_Q_INVALID:       return "INVALID";
    case EMS_Q_OUT_OF_RANGE:  return "OUT_OF_RANGE";
    case EMS_Q_NOT_SUPPORTED: return "NOT_SUPPORTED";
    case EMS_Q_COUNT:
    default:                  return "UNKNOWN";
    }
}

const char *ems_unit_symbol(ems_unit_t unit)
{
    switch (unit) {
    case EMS_UNIT_NONE:  return "-";
    case EMS_UNIT_V:     return "V";
    case EMS_UNIT_A:     return "A";
    case EMS_UNIT_KW:    return "kW";
    case EMS_UNIT_KVAR:  return "kvar";
    case EMS_UNIT_KVA:   return "kVA";
    case EMS_UNIT_RATIO: return "1";
    case EMS_UNIT_HZ:    return "Hz";
    case EMS_UNIT_KWH:   return "kWh";
    case EMS_UNIT_COUNT:
    default:             return "?";
    }
}

const char *ems_status_name(ems_status_t status)
{
    switch (status) {
    case EMS_OK:                return "EMS_OK";
    case EMS_ERR_INVALID_ARG:   return "EMS_ERR_INVALID_ARG";
    case EMS_ERR_NOT_FOUND:     return "EMS_ERR_NOT_FOUND";
    case EMS_ERR_NO_SPACE:      return "EMS_ERR_NO_SPACE";
    case EMS_ERR_INVALID_STATE: return "EMS_ERR_INVALID_STATE";
    default:                    return "EMS_ERR_UNKNOWN";
    }
}
