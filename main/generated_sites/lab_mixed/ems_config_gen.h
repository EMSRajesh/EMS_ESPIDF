/*
 * GENERATED FILE - DO NOT EDIT.
 * Generator: tools/profile/myems_profile_tool.py 1.1.0
 * Site:      lab_mixed.site.json (LAB_MIXED), target sim
 * Inputs:    site:LAB_MIXED; profile:AC_METER_3PH_SIM@1.1.0; profile:DC_METER_SIM@1.0.0; map:MYEMS_SIM_ACMETER_3PH@1.1.0; map:MYEMS_SIM_DCMETER@1.0.0; umd:1.1.0
 * Inputs sha256 (first 16): bf14468aa14bd210
 * Config CRC32: 0xC04114E4   Classification: SIMULATION_ONLY
 * Regenerate: python tools/profile/myems_profile_tool.py generate --site config/site/lab_mixed.site.json --target sim --out-dir main/generated_sites/lab_mixed
 * SIMULATION/TEST CONFIGURATION: synthetic register addresses, never use with real equipment.
 */
#ifndef EMS_CONFIG_GEN_H
#define EMS_CONFIG_GEN_H

#include "ems_config_types.h"

#define EMS_CONFIG_GEN_CRC32 0xC04114E4u
#define EMS_CONFIG_GEN_SITE_ID "LAB_MIXED"
#define EMS_CONFIG_GEN_DEVICE_COUNT 2u

extern const ems_config_build_t ems_config_build;

#endif /* EMS_CONFIG_GEN_H */
