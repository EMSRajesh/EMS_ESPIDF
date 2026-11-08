/*
 * GENERATED FILE - DO NOT EDIT.
 * Source:    config/umd/umd_v1.json
 * UMD:       1.1.0  (sha256 a34247c496d3c0c0)
 * Generator: tools/profile/umd_gen.py 1.1.0
 * Regenerate: python tools/profile/umd_gen.py
 */
#ifndef EMS_PARAM_IDS_H
#define EMS_PARAM_IDS_H

#include <stdint.h>

#define EMS_UMD_VERSION "1.1.0"
#define EMS_UMD_SHA256_PREFIX "a34247c496d3c0c0"
#define EMS_PARAM_COUNT 19u

/* Domain codes: high byte of a runtime ID. */
#define EMS_DOMAIN_DEV 0x00u
#define EMS_DOMAIN_AC 0x01u
#define EMS_DOMAIN_DC 0x02u
#define EMS_DOMAIN_PV 0x03u
#define EMS_DOMAIN_BAT 0x04u
#define EMS_DOMAIN_PCS 0x05u

/* Parameter runtime IDs (permanent). */
#define EMS_PARAM_DEV_STATUS_CODE         ((uint16_t)0x0001u)
#define EMS_PARAM_AC_VOLTAGE_L1_N         ((uint16_t)0x0101u)
#define EMS_PARAM_AC_VOLTAGE_L2_N         ((uint16_t)0x0102u)
#define EMS_PARAM_AC_VOLTAGE_L3_N         ((uint16_t)0x0103u)
#define EMS_PARAM_AC_CURRENT_L1           ((uint16_t)0x0111u)
#define EMS_PARAM_AC_CURRENT_L2           ((uint16_t)0x0112u)
#define EMS_PARAM_AC_CURRENT_L3           ((uint16_t)0x0113u)
#define EMS_PARAM_AC_POWER_ACTIVE_TOTAL   ((uint16_t)0x0121u)
#define EMS_PARAM_AC_POWER_REACTIVE_TOTAL ((uint16_t)0x0122u)
#define EMS_PARAM_AC_POWER_APPARENT_TOTAL ((uint16_t)0x0123u)
#define EMS_PARAM_AC_POWER_FACTOR_TOTAL   ((uint16_t)0x0124u)
#define EMS_PARAM_AC_FREQUENCY            ((uint16_t)0x0130u)
#define EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE ((uint16_t)0x0140u)
#define EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE ((uint16_t)0x0141u)
#define EMS_PARAM_DC_VOLTAGE              ((uint16_t)0x0201u)
#define EMS_PARAM_DC_CURRENT              ((uint16_t)0x0211u)
#define EMS_PARAM_DC_POWER                ((uint16_t)0x0221u)
#define EMS_PARAM_DC_ENERGY_IMPORT        ((uint16_t)0x0240u)
#define EMS_PARAM_DC_ENERGY_EXPORT        ((uint16_t)0x0241u)

#endif /* EMS_PARAM_IDS_H */
