/*
 * GENERATED FILE - DO NOT EDIT.
 * Generator: tools/profile/myems_profile_tool.py 1.0.0
 * Site:      lab_decoder_test.site.json (LAB_DECODER_TEST), target sim
 * Inputs:    site:LAB_DECODER_TEST; profile:DECODER_TEST_SIM@1.1.0; map:MYEMS_SIM_DECODER_TEST@1.1.0; umd:1.0.0
 * Inputs sha256 (first 16): 2289b824dffd5562
 * Config CRC32: 0x5A334BC5   Classification: TEST_ONLY
 * Regenerate: python tools/profile/myems_profile_tool.py generate --site config/site/lab_decoder_test.site.json --target sim
 * SIMULATION/TEST CONFIGURATION: synthetic register addresses, never use with real equipment.
 */
#include "ems_config_gen.h"
#include "ems_param_ids.h"

/* ---- map 0: MYEMS_SIM_DECODER_TEST 1.1.0 (TEST_ONLY) ---- */
static const ems_poll_group_t s_map0_groups[1] = {
    [0] = { .interval_ms = 5000u, .offset_ms = 500u, .stale_timeout_ms = 11000u, .start_address = 1000u, .count = 20u, .function_code = 3u },  /* decoder */
};

static const ems_reg_mapping_t s_map0_mappings[11] = {
    { /* 1: ac.voltage.l1_n @ 1000 FLOAT32 ABCD, V; D1 230.5 */
        .scale = 1.0, .offset = 0.0, .min = -1000000000.0, .max = 1000000000.0,
        .mapping_id = 1u, .param_id = EMS_PARAM_AC_VOLTAGE_L1_N, .address = 1000u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_FLOAT32, .order = EMS_ORDER_ABCD, .flags = 0u },
    { /* 2: ac.voltage.l2_n @ 1002 FLOAT32 CDAB, V; D2 230.5 */
        .scale = 1.0, .offset = 0.0, .min = -1000000000.0, .max = 1000000000.0,
        .mapping_id = 2u, .param_id = EMS_PARAM_AC_VOLTAGE_L2_N, .address = 1002u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_FLOAT32, .order = EMS_ORDER_CDAB, .flags = 0u },
    { /* 3: ac.voltage.l3_n @ 1004 FLOAT32 BADC, V; D3 230.5 */
        .scale = 1.0, .offset = 0.0, .min = -1000000000.0, .max = 1000000000.0,
        .mapping_id = 3u, .param_id = EMS_PARAM_AC_VOLTAGE_L3_N, .address = 1004u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_FLOAT32, .order = EMS_ORDER_BADC, .flags = 0u },
    { /* 4: ac.current.l1 @ 1006 FLOAT32 DCBA, A; D4 230.5 */
        .scale = 1.0, .offset = 0.0, .min = -1000000000.0, .max = 1000000000.0,
        .mapping_id = 4u, .param_id = EMS_PARAM_AC_CURRENT_L1, .address = 1006u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_FLOAT32, .order = EMS_ORDER_DCBA, .flags = 0u },
    { /* 5: ac.power.active.total @ 1008 INT32 ABCD, kW; D5 -12500 */
        .scale = 1.0, .offset = 0.0, .min = -1000000000.0, .max = 1000000000.0,
        .mapping_id = 5u, .param_id = EMS_PARAM_AC_POWER_ACTIVE_TOTAL, .address = 1008u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_INT32, .order = EMS_ORDER_ABCD, .flags = 0u },
    { /* 6: ac.power.reactive.total @ 1010 INT32 CDAB, kvar; D6 -12500 */
        .scale = 1.0, .offset = 0.0, .min = -1000000000.0, .max = 1000000000.0,
        .mapping_id = 6u, .param_id = EMS_PARAM_AC_POWER_REACTIVE_TOTAL, .address = 1010u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_INT32, .order = EMS_ORDER_CDAB, .flags = 0u },
    { /* 7: ac.energy.import.active @ 1012 UINT32 CDAB, kWh; D7 10025 */
        .scale = 1.0, .offset = 0.0, .min = 0.0, .max = 5000000000.0,
        .mapping_id = 7u, .param_id = EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, .address = 1012u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_UINT32, .order = EMS_ORDER_CDAB, .flags = EMS_MAP_F_COUNTER },
    { /* 8: ac.current.l2 @ 1014 FLOAT32 ABCD, A; D8 NaN -> INVALID */
        .scale = 1.0, .offset = 0.0, .min = -1000000000.0, .max = 1000000000.0,
        .mapping_id = 8u, .param_id = EMS_PARAM_AC_CURRENT_L2, .address = 1014u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_FLOAT32, .order = EMS_ORDER_ABCD, .flags = 0u },
    { /* 9: ac.current.l3 @ 1016 FLOAT32 ABCD, A; D9 +Inf -> INVALID */
        .scale = 1.0, .offset = 0.0, .min = -1000000000.0, .max = 1000000000.0,
        .mapping_id = 9u, .param_id = EMS_PARAM_AC_CURRENT_L3, .address = 1016u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_FLOAT32, .order = EMS_ORDER_ABCD, .flags = 0u },
    { /* 10: ac.power.factor.total @ 1018 INT16 AB, 1; D10 -32768 */
        .scale = 1.0, .offset = 0.0, .min = -40000.0, .max = 40000.0,
        .mapping_id = 10u, .param_id = EMS_PARAM_AC_POWER_FACTOR_TOTAL, .address = 1018u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_INT16, .order = EMS_ORDER_AB, .flags = 0u },
    { /* 11: dev.status.code @ 1019 UINT16 AB, -; D11 65535 */
        .scale = 1.0, .offset = 0.0, .min = 0.0, .max = 65535.0,
        .mapping_id = 11u, .param_id = EMS_PARAM_DEV_STATUS_CODE, .address = 1019u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_UINT16, .order = EMS_ORDER_AB, .flags = 0u },
};

static const ems_cfg_map_t s_maps[1] = {
    [0] = { .map_id = "MYEMS_SIM_DECODER_TEST", .map_version = "1.1.0", .groups = s_map0_groups, .mappings = s_map0_mappings, .mapping_count = 11u, .group_count = 1u, .classification = EMS_CLASS_TEST_ONLY },
};

static const ems_cfg_profile_t s_profiles[1] = {
    [0] = { .profile_id = "DECODER_TEST_SIM", .profile_version = "1.1.0", .device_type = "TEST_DEVICE",
            .counter_max_rate_per_hour = 1000000000.0, .map_index = 0u, .classification = EMS_CLASS_TEST_ONLY, .ratio_handling = EMS_RATIO_METER_REPORTS_PRIMARY },
};

static const ems_cfg_bus_t s_buses[1] = {
    [0] = { .bus_id = "RS485_BUS_1", .baud = 9600u, .uart_num = 1u, .tx_gpio = 17u, .rx_gpio = 18u, .rts_gpio = 21u, .parity = EMS_PARITY_EVEN, .stop_bits = 1u },
};

static const ems_cfg_device_t s_devices[1] = {
    [0] = { .instance_id = "DECODER_TEST_01", .location = "LAB",
            .ct_ratio = 1.0, .pt_ratio = 1.0, .probe_interval_ms = 30000u, .timeout_ms = 500u,
            .slot = 0u, .profile_index = 0u, .bus_index = 0u, .slave_id = 1u, .max_retries = 1u, .comm_lost_threshold = 2u,
            .probe_group = 0u /* decoder */, .enabled = true },
};

const ems_config_build_t ems_config_build = {
    .site_id = "LAB_DECODER_TEST",
    .tool_version = "1.0.0",
    .umd_version = "1.0.0",
    .inputs_sha256 = "2289b824dffd5562",
    .inputs_summary = "site:LAB_DECODER_TEST; profile:DECODER_TEST_SIM@1.1.0; map:MYEMS_SIM_DECODER_TEST@1.1.0; umd:1.0.0",
    .buses = s_buses, .maps = s_maps, .profiles = s_profiles, .devices = s_devices,
    .crc32 = 0x5A334BC5u,
    .bus_count = 1u, .map_count = 1u, .profile_count = 1u, .device_count = 1u,
    .target = EMS_TARGET_SIM, .classification = EMS_CLASS_TEST_ONLY,
};
