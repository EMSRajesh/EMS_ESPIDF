/*
 * GENERATED FILE - DO NOT EDIT.
 * Generator: tools/profile/myems_profile_tool.py 1.0.0
 * Site:      lab_ratio_ct200.site.json (LAB_RATIO_CT200), target sim
 * Inputs:    site:LAB_RATIO_CT200; profile:AC_METER_3PH_SIM@1.1.0; map:MYEMS_SIM_ACMETER_3PH@1.1.0; umd:1.0.0
 * Inputs sha256 (first 16): 03aabaa0daa5913c
 * Config CRC32: 0x5A2CC286   Classification: SIMULATION_ONLY
 * Regenerate: python tools/profile/myems_profile_tool.py generate --site config/site/lab_ratio_ct200.site.json --target sim --out-dir main/generated_sites/lab_ratio_ct200
 * SIMULATION/TEST CONFIGURATION: synthetic register addresses, never use with real equipment.
 */
#include "ems_config_gen.h"
#include "ems_param_ids.h"

/* ---- map 0: MYEMS_SIM_ACMETER_3PH 1.1.0 (SIMULATION_ONLY) ---- */
static const ems_poll_group_t s_map0_groups[3] = {
    [0] = { .interval_ms = 5000u, .offset_ms = 500u, .stale_timeout_ms = 11000u, .start_address = 0u, .count = 14u, .function_code = 3u },  /* electrical */
    [1] = { .interval_ms = 10000u, .offset_ms = 1500u, .stale_timeout_ms = 21000u, .start_address = 14u, .count = 4u, .function_code = 3u },  /* energy */
    [2] = { .interval_ms = 30000u, .offset_ms = 2500u, .stale_timeout_ms = 61000u, .start_address = 18u, .count = 1u, .function_code = 3u },  /* status */
};

static const ems_reg_mapping_t s_map0_mappings[14] = {
    { /* 1: ac.voltage.l1_n @ 0 UINT16 AB, V */
        .scale = 0.1, .offset = 0.0, .min = 0.0, .max = 500.0,
        .mapping_id = 1u, .param_id = EMS_PARAM_AC_VOLTAGE_L1_N, .address = 0u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_UINT16, .order = EMS_ORDER_AB, .flags = EMS_MAP_F_APPLY_PT },
    { /* 2: ac.voltage.l2_n @ 1 UINT16 AB, V */
        .scale = 0.1, .offset = 0.0, .min = 0.0, .max = 500.0,
        .mapping_id = 2u, .param_id = EMS_PARAM_AC_VOLTAGE_L2_N, .address = 1u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_UINT16, .order = EMS_ORDER_AB, .flags = EMS_MAP_F_APPLY_PT },
    { /* 3: ac.voltage.l3_n @ 2 UINT16 AB, V */
        .scale = 0.1, .offset = 0.0, .min = 0.0, .max = 500.0,
        .mapping_id = 3u, .param_id = EMS_PARAM_AC_VOLTAGE_L3_N, .address = 2u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_UINT16, .order = EMS_ORDER_AB, .flags = EMS_MAP_F_APPLY_PT },
    { /* 4: ac.current.l1 @ 3 UINT16 AB, A */
        .scale = 0.01, .offset = 0.0, .min = 0.0, .max = 650.0,
        .mapping_id = 4u, .param_id = EMS_PARAM_AC_CURRENT_L1, .address = 3u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_UINT16, .order = EMS_ORDER_AB, .flags = EMS_MAP_F_APPLY_CT },
    { /* 5: ac.current.l2 @ 4 UINT16 AB, A */
        .scale = 0.01, .offset = 0.0, .min = 0.0, .max = 650.0,
        .mapping_id = 5u, .param_id = EMS_PARAM_AC_CURRENT_L2, .address = 4u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_UINT16, .order = EMS_ORDER_AB, .flags = EMS_MAP_F_APPLY_CT },
    { /* 6: ac.current.l3 @ 5 UINT16 AB, A */
        .scale = 0.01, .offset = 0.0, .min = 0.0, .max = 650.0,
        .mapping_id = 6u, .param_id = EMS_PARAM_AC_CURRENT_L3, .address = 5u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_UINT16, .order = EMS_ORDER_AB, .flags = EMS_MAP_F_APPLY_CT },
    { /* 7: ac.power.active.total @ 6 INT32 ABCD, kW */
        .scale = 0.001, .offset = 0.0, .min = -1000.0, .max = 1000.0,
        .mapping_id = 7u, .param_id = EMS_PARAM_AC_POWER_ACTIVE_TOTAL, .address = 6u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_INT32, .order = EMS_ORDER_ABCD, .flags = EMS_MAP_F_APPLY_CT | EMS_MAP_F_APPLY_PT },
    { /* 8: ac.power.reactive.total @ 8 INT32 ABCD, kvar */
        .scale = 0.001, .offset = 0.0, .min = -1000.0, .max = 1000.0,
        .mapping_id = 8u, .param_id = EMS_PARAM_AC_POWER_REACTIVE_TOTAL, .address = 8u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_INT32, .order = EMS_ORDER_ABCD, .flags = EMS_MAP_F_APPLY_CT | EMS_MAP_F_APPLY_PT },
    { /* 9: ac.power.apparent.total @ 10 UINT32 ABCD, kVA */
        .scale = 0.001, .offset = 0.0, .min = 0.0, .max = 1000.0,
        .mapping_id = 9u, .param_id = EMS_PARAM_AC_POWER_APPARENT_TOTAL, .address = 10u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_UINT32, .order = EMS_ORDER_ABCD, .flags = EMS_MAP_F_APPLY_CT | EMS_MAP_F_APPLY_PT },
    { /* 10: ac.power.factor.total @ 12 INT16 AB, 1 */
        .scale = 0.001, .offset = 0.0, .min = -1.0, .max = 1.0,
        .mapping_id = 10u, .param_id = EMS_PARAM_AC_POWER_FACTOR_TOTAL, .address = 12u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_INT16, .order = EMS_ORDER_AB, .flags = 0u },
    { /* 11: ac.frequency @ 13 UINT16 AB, Hz */
        .scale = 0.01, .offset = 0.0, .min = 45.0, .max = 65.0,
        .mapping_id = 11u, .param_id = EMS_PARAM_AC_FREQUENCY, .address = 13u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_UINT16, .order = EMS_ORDER_AB, .flags = 0u },
    { /* 12: ac.energy.import.active @ 14 UINT32 ABCD, kWh */
        .scale = 0.01, .offset = 0.0, .min = 0.0, .max = 43000000.0,
        .mapping_id = 12u, .param_id = EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, .address = 14u, .bit_mask = 0x0000u,
        .group_index = 1u, .type = EMS_REG_UINT32, .order = EMS_ORDER_ABCD, .flags = EMS_MAP_F_APPLY_CT | EMS_MAP_F_APPLY_PT | EMS_MAP_F_COUNTER },
    { /* 13: ac.energy.export.active @ 16 UINT32 ABCD, kWh */
        .scale = 0.01, .offset = 0.0, .min = 0.0, .max = 43000000.0,
        .mapping_id = 13u, .param_id = EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE, .address = 16u, .bit_mask = 0x0000u,
        .group_index = 1u, .type = EMS_REG_UINT32, .order = EMS_ORDER_ABCD, .flags = EMS_MAP_F_APPLY_CT | EMS_MAP_F_APPLY_PT | EMS_MAP_F_COUNTER },
    { /* 14: dev.status.code @ 18 UINT16 AB, - */
        .scale = 1.0, .offset = 0.0, .min = 0.0, .max = 65535.0,
        .mapping_id = 14u, .param_id = EMS_PARAM_DEV_STATUS_CODE, .address = 18u, .bit_mask = 0x0000u,
        .group_index = 2u, .type = EMS_REG_UINT16, .order = EMS_ORDER_AB, .flags = 0u },
};

static const ems_cfg_map_t s_maps[1] = {
    [0] = { .map_id = "MYEMS_SIM_ACMETER_3PH", .map_version = "1.1.0", .groups = s_map0_groups, .mappings = s_map0_mappings, .mapping_count = 14u, .group_count = 3u, .classification = EMS_CLASS_SIMULATION_ONLY },
};

static const ems_cfg_profile_t s_profiles[1] = {
    [0] = { .profile_id = "AC_METER_3PH_SIM", .profile_version = "1.1.0", .device_type = "AC_METER",
            .counter_max_rate_per_hour = 1000.0, .map_index = 0u, .classification = EMS_CLASS_SIMULATION_ONLY, .ratio_handling = EMS_RATIO_MYEMS_APPLIES },
};

static const ems_cfg_bus_t s_buses[1] = {
    [0] = { .bus_id = "RS485_BUS_1", .baud = 9600u, .uart_num = 1u, .tx_gpio = 17u, .rx_gpio = 18u, .rts_gpio = 21u, .parity = EMS_PARITY_EVEN, .stop_bits = 1u },
};

static const ems_cfg_device_t s_devices[1] = {
    [0] = { .instance_id = "GRID_INCOMER_CT200", .location = "MAIN_INCOMER",
            .ct_ratio = 200.0, .pt_ratio = 1.0, .probe_interval_ms = 30000u, .timeout_ms = 500u,
            .slot = 0u, .profile_index = 0u, .bus_index = 0u, .slave_id = 1u, .max_retries = 1u, .comm_lost_threshold = 2u,
            .probe_group = 2u /* status */, .enabled = true },
};

const ems_config_build_t ems_config_build = {
    .site_id = "LAB_RATIO_CT200",
    .tool_version = "1.0.0",
    .umd_version = "1.0.0",
    .inputs_sha256 = "03aabaa0daa5913c",
    .inputs_summary = "site:LAB_RATIO_CT200; profile:AC_METER_3PH_SIM@1.1.0; map:MYEMS_SIM_ACMETER_3PH@1.1.0; umd:1.0.0",
    .buses = s_buses, .maps = s_maps, .profiles = s_profiles, .devices = s_devices,
    .crc32 = 0x5A2CC286u,
    .bus_count = 1u, .map_count = 1u, .profile_count = 1u, .device_count = 1u,
    .target = EMS_TARGET_SIM, .classification = EMS_CLASS_SIMULATION_ONLY,
};
