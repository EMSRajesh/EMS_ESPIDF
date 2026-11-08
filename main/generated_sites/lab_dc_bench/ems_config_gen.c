/*
 * GENERATED FILE - DO NOT EDIT.
 * Generator: tools/profile/myems_profile_tool.py 1.1.0
 * Site:      lab_dc_bench.site.json (LAB_DC_BENCH), target sim
 * Inputs:    site:LAB_DC_BENCH; profile:DC_METER_SIM@1.0.0; map:MYEMS_SIM_DCMETER@1.0.0; umd:1.1.0
 * Inputs sha256 (first 16): 49328f2c842a4fd2
 * Config CRC32: 0xCC22DC64   Classification: SIMULATION_ONLY
 * Regenerate: python tools/profile/myems_profile_tool.py generate --site config/site/lab_dc_bench.site.json --target sim --out-dir main/generated_sites/lab_dc_bench
 * SIMULATION/TEST CONFIGURATION: synthetic register addresses, never use with real equipment.
 */
#include "ems_config_gen.h"
#include "ems_param_ids.h"

/* ---- map 0: MYEMS_SIM_DCMETER 1.0.0 (SIMULATION_ONLY) ---- */
static const ems_poll_group_t s_map0_groups[3] = {
    [0] = { .interval_ms = 4000u, .offset_ms = 300u, .stale_timeout_ms = 9000u, .start_address = 0u, .count = 6u, .function_code = 4u },  /* measurements */
    [1] = { .interval_ms = 10000u, .offset_ms = 1200u, .stale_timeout_ms = 21000u, .start_address = 100u, .count = 4u, .function_code = 4u },  /* energy */
    [2] = { .interval_ms = 30000u, .offset_ms = 2200u, .stale_timeout_ms = 61000u, .start_address = 200u, .count = 1u, .function_code = 3u },  /* status */
};

static const ems_reg_mapping_t s_map0_mappings[6] = {
    { /* 1: dc.voltage @ 0 FLOAT32 ABCD, V; N1 800.0 V: 4448 0000 */
        .scale = 1.0, .offset = 0.0, .min = 0.0, .max = 1500.0,
        .mapping_id = 1u, .param_id = EMS_PARAM_DC_VOLTAGE, .address = 0u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_FLOAT32, .order = EMS_ORDER_ABCD, .flags = 0u },
    { /* 2: dc.current @ 2 INT32 CDAB, A; N2 125.0 A: E848 0001 */
        .scale = 0.001, .offset = 0.0, .min = -2000.0, .max = 2000.0,
        .mapping_id = 2u, .param_id = EMS_PARAM_DC_CURRENT, .address = 2u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_INT32, .order = EMS_ORDER_CDAB, .flags = 0u },
    { /* 3: dc.power @ 4 FLOAT32 CDAB, kW; N3 100.0 kW: 0000 42C8 */
        .scale = 1.0, .offset = 0.0, .min = -3000.0, .max = 3000.0,
        .mapping_id = 3u, .param_id = EMS_PARAM_DC_POWER, .address = 4u, .bit_mask = 0x0000u,
        .group_index = 0u, .type = EMS_REG_FLOAT32, .order = EMS_ORDER_CDAB, .flags = 0u },
    { /* 4: dc.energy.import @ 100 UINT32 CDAB, kWh; N4 2500.0 kWh: D090 0003 */
        .scale = 0.01, .offset = 0.0, .min = 0.0, .max = 43000000.0,
        .mapping_id = 4u, .param_id = EMS_PARAM_DC_ENERGY_IMPORT, .address = 100u, .bit_mask = 0x0000u,
        .group_index = 1u, .type = EMS_REG_UINT32, .order = EMS_ORDER_CDAB, .flags = EMS_MAP_F_COUNTER },
    { /* 5: dc.energy.export @ 102 UINT32 CDAB, kWh; N5 0.0 kWh: 0000 0000 */
        .scale = 0.01, .offset = 0.0, .min = 0.0, .max = 43000000.0,
        .mapping_id = 5u, .param_id = EMS_PARAM_DC_ENERGY_EXPORT, .address = 102u, .bit_mask = 0x0000u,
        .group_index = 1u, .type = EMS_REG_UINT32, .order = EMS_ORDER_CDAB, .flags = EMS_MAP_F_COUNTER },
    { /* 6: dev.status.code @ 200 UINT16 AB, -; N6 0 -: 0000 */
        .scale = 1.0, .offset = 0.0, .min = 0.0, .max = 65535.0,
        .mapping_id = 6u, .param_id = EMS_PARAM_DEV_STATUS_CODE, .address = 200u, .bit_mask = 0x0000u,
        .group_index = 2u, .type = EMS_REG_UINT16, .order = EMS_ORDER_AB, .flags = 0u },
};

static const ems_cfg_map_t s_maps[1] = {
    [0] = { .map_id = "MYEMS_SIM_DCMETER", .map_version = "1.0.0", .groups = s_map0_groups, .mappings = s_map0_mappings, .mapping_count = 6u, .group_count = 3u, .classification = EMS_CLASS_SIMULATION_ONLY },
};

static const ems_cfg_profile_t s_profiles[1] = {
    [0] = { .profile_id = "DC_METER_SIM", .profile_version = "1.0.0", .device_type = "DC_METER",
            .counter_max_rate_per_hour = 3000.0, .map_index = 0u, .classification = EMS_CLASS_SIMULATION_ONLY, .ratio_handling = EMS_RATIO_METER_REPORTS_PRIMARY },
};

static const ems_cfg_bus_t s_buses[1] = {
    [0] = { .bus_id = "RS485_BUS_1", .baud = 9600u, .uart_num = 1u, .tx_gpio = 17u, .rx_gpio = 18u, .rts_gpio = 21u, .parity = EMS_PARITY_EVEN, .stop_bits = 1u },
};

static const ems_cfg_device_t s_devices[1] = {
    [0] = { .instance_id = "DC_BATT_STRING_01", .location = "BATTERY_STRING_1",
            .ct_ratio = 1.0, .pt_ratio = 1.0, .probe_interval_ms = 30000u, .timeout_ms = 500u,
            .slot = 0u, .profile_index = 0u, .bus_index = 0u, .slave_id = 3u, .max_retries = 1u, .comm_lost_threshold = 2u,
            .probe_group = 2u /* status */, .enabled = true },
};

const ems_config_build_t ems_config_build = {
    .site_id = "LAB_DC_BENCH",
    .tool_version = "1.1.0",
    .umd_version = "1.1.0",
    .inputs_sha256 = "49328f2c842a4fd2",
    .inputs_summary = "site:LAB_DC_BENCH; profile:DC_METER_SIM@1.0.0; map:MYEMS_SIM_DCMETER@1.0.0; umd:1.1.0",
    .buses = s_buses, .maps = s_maps, .profiles = s_profiles, .devices = s_devices,
    .crc32 = 0xCC22DC64u,
    .bus_count = 1u, .map_count = 1u, .profile_count = 1u, .device_count = 1u,
    .target = EMS_TARGET_SIM, .classification = EMS_CLASS_SIMULATION_ONLY,
};
