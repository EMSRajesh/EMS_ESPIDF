/*
 * Firmware activation checks (DP-WP4 D7): CFG-017 (altered byte -> CRC),
 * CFG-018 (target/classification), CFG-019 (valid), CFG-020 (mapping outside
 * its group, CRC recomputed), and every other firmware-side rule.
 * Uses the generated lab_bench tables and modified copies of them.
 */
#include <string.h>
#include "unity.h"
#include "ems_config_gen.h"
#include "ems_param_ids.h"
#include "ems_profile.h"

static ems_config_build_t C;
static ems_cfg_bus_t      C_bus[2];
static ems_cfg_device_t   C_dev[5];
static ems_cfg_profile_t  C_prof[2];
static ems_cfg_map_t      C_map[2];
static ems_poll_group_t   C_groups[8];
static ems_reg_mapping_t  C_mappings[32];
static ems_activation_report_t R;

static void copy_cfg(void)
{
    const ems_config_build_t *g = &ems_config_build;
    C = *g;
    memcpy(C_bus, g->buses, sizeof(C_bus[0]) * g->bus_count);
    memcpy(C_dev, g->devices, sizeof(C_dev[0]) * g->device_count);
    memcpy(C_prof, g->profiles, sizeof(C_prof[0]) * g->profile_count);
    memcpy(C_map, g->maps, sizeof(C_map[0]) * g->map_count);
    memcpy(C_groups, g->maps[0].groups, sizeof(C_groups[0]) * g->maps[0].group_count);
    memcpy(C_mappings, g->maps[0].mappings, sizeof(C_mappings[0]) * g->maps[0].mapping_count);
    C.buses = C_bus;
    C.devices = C_dev;
    C.profiles = C_prof;
    C.maps = C_map;
    C_map[0].groups = C_groups;
    C_map[0].mappings = C_mappings;
}

/* After an edit: recompute the CRC, as a (wrong) regenerated file would have. */
static void reseal(void)
{
    C.crc32 = ems_config_crc32(&C);
}

static void expect(ems_activation_status_t st, const char *rule, bool sim)
{
    TEST_ASSERT_EQUAL_STRING(ems_activation_status_name(st),
                             ems_activation_status_name(ems_profile_activate(&C, sim, &R)));
    TEST_ASSERT_EQUAL_STRING(rule, R.rule);
}

static void test_CFG019_generated_config_activates_in_sim_build(void)
{
    copy_cfg();
    expect(EMS_ACT_OK, "OK", true);
    TEST_ASSERT_EQUAL_HEX32(EMS_CONFIG_GEN_CRC32, R.crc_computed);
    TEST_ASSERT_EQUAL_HEX32(R.crc_stored, R.crc_computed);
    TEST_ASSERT_TRUE(R.sim_build);
    TEST_ASSERT_EQUAL_INT(EMS_ACT_OK, ems_profile_activate(&ems_config_build, true, NULL));
}

static void test_CFG017_altered_byte_fails_crc(void)
{
    copy_cfg();
    C_mappings[3].scale = 0.1;                     /* current l1: 0.01 -> 0.1 */
    expect(EMS_ACT_CRC, "CRC", true);
    TEST_ASSERT_EQUAL_HEX32(EMS_CONFIG_GEN_CRC32, R.crc_stored);
    TEST_ASSERT_NOT_EQUAL(R.crc_stored, R.crc_computed);
    copy_cfg();
    C_dev[0].slave_id = 7u;
    expect(EMS_ACT_CRC, "CRC", true);
    copy_cfg();
    C_groups[1].interval_ms = 20000u;
    expect(EMS_ACT_CRC, "CRC", true);
}

static void test_CFG018_sim_config_in_production_build(void)
{
    copy_cfg();
    expect(EMS_ACT_TARGET, "V-13", false);
}

static void test_CFG018_production_config_in_sim_build(void)
{
    copy_cfg();
    C.target = EMS_TARGET_PROD;
    C.classification = EMS_CLASS_PRODUCTION;
    C_map[0].classification = EMS_CLASS_PRODUCTION;
    C_prof[0].classification = EMS_CLASS_PRODUCTION;
    reseal();
    expect(EMS_ACT_TARGET, "V-13", true);
    expect(EMS_ACT_OK, "OK", false);              /* ... and is accepted by a production build */
}

static void test_production_build_rejects_any_sim_part(void)
{
    copy_cfg();
    C.target = EMS_TARGET_PROD;
    C.classification = EMS_CLASS_PRODUCTION;
    C_prof[0].classification = EMS_CLASS_PRODUCTION;   /* map still SIMULATION_ONLY */
    reseal();
    expect(EMS_ACT_TARGET, "V-13", false);
    copy_cfg();
    C.target = EMS_TARGET_PROD;
    C.classification = EMS_CLASS_PRODUCTION;
    C_map[0].classification = EMS_CLASS_PRODUCTION;    /* profile still SIMULATION_ONLY */
    reseal();
    expect(EMS_ACT_TARGET, "V-13", false);
}

static void test_CFG020_mapping_outside_group(void)
{
    copy_cfg();
    C_mappings[13].address = 19u;                  /* status group is 18..18 */
    reseal();
    expect(EMS_ACT_MAP, "V-06", true);
    TEST_ASSERT_EQUAL_UINT8(0u, R.index);
    TEST_ASSERT_EQUAL_UINT8(13u, R.sub_index);
    TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_OUTSIDE_BLOCK, R.map_check);
    copy_cfg();
    C_mappings[6].address = 13u;                   /* INT32 at 13..14 crosses the electrical block */
    reseal();
    expect(EMS_ACT_MAP, "V-06", true);
}

static void test_map_rules(void)
{
    copy_cfg(); C_groups[0].function_code = 6u; reseal();            expect(EMS_ACT_MAP, "V-07", true);
    copy_cfg(); C_groups[0].count = 126u; reseal();                  expect(EMS_ACT_MAP, "V-07", true);
    copy_cfg(); C_groups[0].interval_ms = 500u; reseal();            expect(EMS_ACT_MAP, "V-09", true);
    copy_cfg(); C_groups[1].stale_timeout_ms = 10000u; reseal();     expect(EMS_ACT_MAP, "V-09", true);
    copy_cfg(); C_mappings[2].scale = 0.0; reseal();                 expect(EMS_ACT_MAP, "V-08", true);
    copy_cfg(); C_mappings[2].min = 600.0; reseal();                 expect(EMS_ACT_MAP, "V-08", true);
    copy_cfg(); C_mappings[2].order = EMS_ORDER_CDAB; reseal();      expect(EMS_ACT_MAP, "V-05", true);
    copy_cfg(); C_mappings[4].mapping_id = 1u; reseal();             expect(EMS_ACT_MAP, "V-02", true);
    copy_cfg(); C_mappings[0].group_index = 7u; reseal();            expect(EMS_ACT_MAP, "V-06", true);
}

static void test_dictionary_rules(void)
{
    copy_cfg(); C_mappings[2].param_id = 0x0199u; reseal();          expect(EMS_ACT_DICTIONARY, "V-04", true);
    copy_cfg(); C_mappings[2].param_id = EMS_PARAM_AC_VOLTAGE_L1_N; reseal();
    expect(EMS_ACT_DICTIONARY, "V-02", true);
    TEST_ASSERT_EQUAL_UINT8(2u, R.sub_index);
}

static void test_reference_rules(void)
{
    copy_cfg(); C_prof[0].map_index = 3u; reseal();                  expect(EMS_ACT_REFERENCE, "V-03", true);
    copy_cfg(); C_dev[0].profile_index = 2u; reseal();               expect(EMS_ACT_REFERENCE, "V-03", true);
    copy_cfg(); C_dev[0].bus_index = 1u; reseal();                   expect(EMS_ACT_REFERENCE, "V-03", true);
}

static void test_device_and_bus_rules(void)
{
    copy_cfg(); C_dev[0].slave_id = 0u; reseal();                    expect(EMS_ACT_DEVICE, "V-10", true);
    copy_cfg(); C_dev[0].slave_id = 248u; reseal();                  expect(EMS_ACT_DEVICE, "V-10", true);
    copy_cfg(); C_dev[0].timeout_ms = 20u; reseal();                 expect(EMS_ACT_DEVICE, "V-10", true);
    copy_cfg(); C_dev[0].max_retries = 6u; reseal();                 expect(EMS_ACT_DEVICE, "V-10", true);
    copy_cfg(); C_dev[0].comm_lost_threshold = 0u; reseal();         expect(EMS_ACT_DEVICE, "V-10", true);
    copy_cfg(); C_dev[0].probe_group = 3u; reseal();                 expect(EMS_ACT_DEVICE, "V-10", true);
    copy_cfg(); C_dev[0].probe_interval_ms = 500u; reseal();         expect(EMS_ACT_DEVICE, "V-10", true);
    copy_cfg(); C_dev[0].ct_ratio = 0.0; reseal();                   expect(EMS_ACT_DEVICE, "V-10", true);
    copy_cfg(); C_dev[0].slot = 1u; reseal();                        expect(EMS_ACT_DEVICE, "V-02", true);
    copy_cfg(); C_bus[0].baud = 300u; reseal();                      expect(EMS_ACT_BUS, "V-10", true);
    copy_cfg(); C_bus[0].stop_bits = 3u; reseal();                   expect(EMS_ACT_BUS, "V-10", true);
}

static void test_duplicate_slave_on_bus(void)
{
    copy_cfg();
    C_dev[1] = C_dev[0];
    C_dev[1].slot = 1u;
    C.device_count = 2u;
    reseal();
    expect(EMS_ACT_DUPLICATE_SLAVE, "V-10", true);
    TEST_ASSERT_EQUAL_UINT8(1u, R.index);
    C_dev[1].slave_id = 2u;
    reseal();
    expect(EMS_ACT_OK, "OK", true);
}

static void test_V12_ratio_rule(void)
{
    copy_cfg();
    C_prof[0].ratio_handling = EMS_RATIO_METER_REPORTS_PRIMARY;
    C_dev[0].ct_ratio = 200.0;
    reseal();
    expect(EMS_ACT_RATIO, "V-12", true);
    C_dev[0].ct_ratio = 1.0;
    reseal();
    expect(EMS_ACT_OK, "OK", true);
}

static void test_V15_limits(void)
{
    /* Counts are checked before the CRC is computed, so a corrupted count is
     * reported as V-15 and never makes the CRC read past a table (no reseal:
     * computing the CRC here would itself read out of bounds). */
    copy_cfg(); C.device_count = 5u;                                 expect(EMS_ACT_LIMITS, "V-15", true);
    copy_cfg(); C.device_count = 0u;                                 expect(EMS_ACT_LIMITS, "V-15", true);
    copy_cfg(); C_map[0].group_count = 9u;                           expect(EMS_ACT_LIMITS, "V-15", true);
    copy_cfg(); C_map[0].mapping_count = 200u;                       expect(EMS_ACT_LIMITS, "V-15", true);
    copy_cfg(); C.bus_count = 3u;                                    expect(EMS_ACT_LIMITS, "V-15", true);
    copy_cfg(); C_map[0].mappings = NULL;                            expect(EMS_ACT_NULL, "V-01", true);
    /* 4 devices x 3 groups = 12 entries is fine; the limit (16) is checked */
    copy_cfg();
    for (uint8_t i = 1u; i < 4u; i++) {
        C_dev[i] = C_dev[0];
        C_dev[i].slot = i;
        C_dev[i].slave_id = (uint8_t)(i + 1u);
    }
    C.device_count = 4u;
    reseal();
    expect(EMS_ACT_OK, "OK", true);
}

/* V-15 poll-entry limit: 4 devices x 4 groups = 16 accepted, x 5 = 20 rejected. */
static void test_V15_poll_entry_limit(void)
{
    for (uint8_t groups = 4u; groups <= 5u; groups++) {
        copy_cfg();
        for (uint8_t g = 3u; g < groups; g++) {
            C_groups[g] = C_groups[2];               /* extra groups after the status group */
            C_groups[g].start_address = (uint16_t)(100u + g);
        }
        C_map[0].group_count = groups;
        for (uint8_t i = 1u; i < 4u; i++) {
            C_dev[i] = C_dev[0];
            C_dev[i].slot = i;
            C_dev[i].slave_id = (uint8_t)(i + 1u);
        }
        C.device_count = 4u;
        reseal();
        if (groups == 4u) {
            expect(EMS_ACT_OK, "OK", true);
        } else {
            expect(EMS_ACT_LIMITS, "V-15", true);
        }
    }
    /* a disabled device does not count */
    C_dev[3].enabled = false;
    reseal();
    expect(EMS_ACT_OK, "OK", true);
}

static void test_null_tables(void)
{
    TEST_ASSERT_EQUAL_INT(EMS_ACT_NULL, ems_profile_activate(NULL, true, &R));
    TEST_ASSERT_EQUAL_STRING("V-01", R.rule);
    copy_cfg();
    C.devices = NULL;
    expect(EMS_ACT_NULL, "V-01", true);
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", ems_activation_status_name((ems_activation_status_t)99));
}

void run_profile_activate_tests(void)
{
    RUN_TEST(test_CFG019_generated_config_activates_in_sim_build);
    RUN_TEST(test_CFG017_altered_byte_fails_crc);
    RUN_TEST(test_CFG018_sim_config_in_production_build);
    RUN_TEST(test_CFG018_production_config_in_sim_build);
    RUN_TEST(test_production_build_rejects_any_sim_part);
    RUN_TEST(test_CFG020_mapping_outside_group);
    RUN_TEST(test_map_rules);
    RUN_TEST(test_dictionary_rules);
    RUN_TEST(test_reference_rules);
    RUN_TEST(test_device_and_bus_rules);
    RUN_TEST(test_duplicate_slave_on_bus);
    RUN_TEST(test_V12_ratio_rule);
    RUN_TEST(test_V15_limits);
    RUN_TEST(test_V15_poll_entry_limit);
    RUN_TEST(test_null_tables);
}
