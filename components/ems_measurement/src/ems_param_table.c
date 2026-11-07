/*
 * GENERATED FILE - DO NOT EDIT.
 * Source:    config/umd/umd_v1.json
 * UMD:       1.0.0  (sha256 35872195dd87c1aa)
 * Generator: tools/profile/umd_gen.py 1.0.0
 */
#include "ems_measurement.h"
#include "ems_param_ids.h"

/* Sorted by runtime ID. */
const ems_param_info_t ems_param_table[EMS_PARAM_COUNT] = {
    { EMS_PARAM_DEV_STATUS_CODE, EMS_UNIT_NONE, EMS_VT_ENUM, "dev.status.code", "Raw device status word" },
    { EMS_PARAM_AC_VOLTAGE_L1_N, EMS_UNIT_V, EMS_VT_FLOAT, "ac.voltage.l1_n", "Phase L1-N RMS voltage" },
    { EMS_PARAM_AC_VOLTAGE_L2_N, EMS_UNIT_V, EMS_VT_FLOAT, "ac.voltage.l2_n", "Phase L2-N RMS voltage" },
    { EMS_PARAM_AC_VOLTAGE_L3_N, EMS_UNIT_V, EMS_VT_FLOAT, "ac.voltage.l3_n", "Phase L3-N RMS voltage" },
    { EMS_PARAM_AC_CURRENT_L1, EMS_UNIT_A, EMS_VT_FLOAT, "ac.current.l1", "Phase L1 RMS current" },
    { EMS_PARAM_AC_CURRENT_L2, EMS_UNIT_A, EMS_VT_FLOAT, "ac.current.l2", "Phase L2 RMS current" },
    { EMS_PARAM_AC_CURRENT_L3, EMS_UNIT_A, EMS_VT_FLOAT, "ac.current.l3", "Phase L3 RMS current" },
    { EMS_PARAM_AC_POWER_ACTIVE_TOTAL, EMS_UNIT_KW, EMS_VT_FLOAT, "ac.power.active.total", "Total active power, import positive" },
    { EMS_PARAM_AC_POWER_REACTIVE_TOTAL, EMS_UNIT_KVAR, EMS_VT_FLOAT, "ac.power.reactive.total", "Total reactive power" },
    { EMS_PARAM_AC_POWER_APPARENT_TOTAL, EMS_UNIT_KVA, EMS_VT_FLOAT, "ac.power.apparent.total", "Total apparent power" },
    { EMS_PARAM_AC_POWER_FACTOR_TOTAL, EMS_UNIT_RATIO, EMS_VT_FLOAT, "ac.power.factor.total", "Total power factor, sign follows active power" },
    { EMS_PARAM_AC_FREQUENCY, EMS_UNIT_HZ, EMS_VT_FLOAT, "ac.frequency", "System frequency" },
    { EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_UNIT_KWH, EMS_VT_COUNTER, "ac.energy.import.active", "Cumulative imported active energy" },
    { EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE, EMS_UNIT_KWH, EMS_VT_COUNTER, "ac.energy.export.active", "Cumulative exported active energy" },
};

const size_t ems_param_table_len = EMS_PARAM_COUNT;
