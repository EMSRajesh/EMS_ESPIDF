/*
 * Per-site generated tables (DP-WP4 D9, DP-WP5 M2): main/generated_sites/<site>/.
 * Built once per site (see CMakeLists.txt); EMS_TEST_SITE_2DEV, _CT200, _DC
 * or _MIXED selects the expectations. The common table checks
 * (CRC, firmware validation, references) come from test_ems_config_common.c.
 */
#include <string.h>
#include "unity.h"
#include "ems_config_gen.h"
#include "ems_profile.h"
#include "ems_sched_core.h"

/* Same activation as the firmware (app_main.c) in a simulation build. */
static void test_CFG019_site_activates_in_a_sim_build_only(void)
{
    ems_activation_report_t rep;
    TEST_ASSERT_EQUAL_INT(EMS_ACT_OK, ems_profile_activate(&ems_config_build, true, &rep));
    TEST_ASSERT_EQUAL_INT(EMS_ACT_TARGET, ems_profile_activate(&ems_config_build, false, &rep));   /* V-13 */
    TEST_ASSERT_EQUAL_STRING("V-13", rep.rule);
}

static void test_site_devices(void)
{
#if defined(EMS_TEST_SITE_2DEV)
    TEST_ASSERT_EQUAL_STRING("LAB_BENCH_02", EMS_CONFIG_GEN_SITE_ID);
    TEST_ASSERT_EQUAL_UINT8(2u, ems_config_build.device_count);
    TEST_ASSERT_EQUAL_STRING("GRID_INCOMER_01", ems_config_build.devices[0].instance_id);
    TEST_ASSERT_EQUAL_STRING("FEEDER_MCC_01", ems_config_build.devices[1].instance_id);
    TEST_ASSERT_EQUAL_UINT8(1u, ems_config_build.devices[0].slave_id);
    TEST_ASSERT_EQUAL_UINT8(2u, ems_config_build.devices[1].slave_id);
    TEST_ASSERT_EQUAL_UINT8(1u, ems_config_build.devices[1].slot);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, ems_config_build.devices[1].ct_ratio);
#elif defined(EMS_TEST_SITE_CT200)
    TEST_ASSERT_EQUAL_STRING("LAB_RATIO_CT200", EMS_CONFIG_GEN_SITE_ID);
    TEST_ASSERT_EQUAL_UINT8(1u, ems_config_build.device_count);
    TEST_ASSERT_EQUAL_STRING("GRID_INCOMER_CT200", ems_config_build.devices[0].instance_id);
    TEST_ASSERT_EQUAL_DOUBLE(200.0, ems_config_build.devices[0].ct_ratio);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, ems_config_build.devices[0].pt_ratio);
#elif defined(EMS_TEST_SITE_DC)
    /* DP-WP5 GEN5-005 / UMD5-004: DC meter alone */
    TEST_ASSERT_EQUAL_STRING("LAB_DC_BENCH", EMS_CONFIG_GEN_SITE_ID);
    TEST_ASSERT_EQUAL_UINT8(1u, ems_config_build.device_count);
    TEST_ASSERT_EQUAL_STRING("DC_BATT_STRING_01", ems_config_build.devices[0].instance_id);
    TEST_ASSERT_EQUAL_UINT8(3u, ems_config_build.devices[0].slave_id);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, ems_config_build.devices[0].ct_ratio);   /* V-12: METER_REPORTS_PRIMARY */
    TEST_ASSERT_EQUAL_DOUBLE(1.0, ems_config_build.devices[0].pt_ratio);
#elif defined(EMS_TEST_SITE_MIXED)
    /* DP-WP5 MIX: AC meter slave 1 + DC meter slave 3 on one bus */
    TEST_ASSERT_EQUAL_STRING("LAB_MIXED", EMS_CONFIG_GEN_SITE_ID);
    TEST_ASSERT_EQUAL_UINT8(2u, ems_config_build.device_count);
    TEST_ASSERT_EQUAL_UINT8(2u, ems_config_build.profile_count);
    TEST_ASSERT_EQUAL_UINT8(2u, ems_config_build.map_count);
    TEST_ASSERT_EQUAL_STRING("GRID_INCOMER_01", ems_config_build.devices[0].instance_id);
    TEST_ASSERT_EQUAL_STRING("DC_BATT_STRING_01", ems_config_build.devices[1].instance_id);
    TEST_ASSERT_EQUAL_UINT8(1u, ems_config_build.devices[0].slave_id);
    TEST_ASSERT_EQUAL_UINT8(3u, ems_config_build.devices[1].slave_id);
    TEST_ASSERT_EQUAL_STRING("AC_METER",
        ems_config_build.profiles[ems_config_build.devices[0].profile_index].device_type);
    TEST_ASSERT_EQUAL_STRING("DC_METER",
        ems_config_build.profiles[ems_config_build.devices[1].profile_index].device_type);
#else
#error "define EMS_TEST_SITE_2DEV, EMS_TEST_SITE_CT200, EMS_TEST_SITE_DC or EMS_TEST_SITE_MIXED"
#endif
    for (uint8_t d = 0; d < ems_config_build.device_count; d++) {
        TEST_ASSERT_TRUE(ems_config_build.devices[d].enabled);
        TEST_ASSERT_EQUAL_UINT8(0u, ems_config_build.devices[d].bus_index);
    }
}

/* DR-24 second device by configuration only: the plan has one entry per
 * (device, poll group), devices contiguous, request IDs route back. */
static void test_plan_from_site_tables(void)
{
    ems_sched_plan_t plan;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_OK, ems_sched_plan_build(&ems_config_build, &plan));
    TEST_ASSERT_EQUAL_UINT8(ems_config_build.device_count, plan.device_count);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(3u * ems_config_build.device_count), plan.entry_count);
    for (uint8_t e = 0; e < plan.entry_count; e++) {
        TEST_ASSERT_EQUAL_UINT8(e / 3u, plan.entries[e].slot);
        TEST_ASSERT_EQUAL_UINT8(ems_config_build.devices[e / 3u].slave_id, plan.entries[e].slave_id);
        TEST_ASSERT_EQUAL_UINT8(e, ems_sched_request_entry(ems_sched_make_request_id(e, 77u)));
    }
    /* T_wc 3.118 s and guard T_wc + 5 s, as in the budget output */
    TEST_ASSERT_TRUE(plan.devices[0].t_wc_us > 3100000 && plan.devices[0].t_wc_us < 3140000);
    TEST_ASSERT_EQUAL_INT64(plan.devices[0].t_wc_us + 5000000, plan.devices[0].guard_us);
}

#if defined(EMS_TEST_SITE_DC) || defined(EMS_TEST_SITE_MIXED)
#include "ems_param_ids.h"

/* The DC meter's profile and map, wherever it sits in the site. */
static const ems_cfg_device_t *dc_device(void)
{
    for (uint8_t d = 0; d < ems_config_build.device_count; d++) {
        const ems_cfg_profile_t *p = &ems_config_build.profiles[ems_config_build.devices[d].profile_index];
        if (strcmp(p->device_type, "DC_METER") == 0) {
            return &ems_config_build.devices[d];
        }
    }
    return NULL;
}

/* DP-WP5 UMD5-001 / UMD5-003 / DCDEC-008 (table part) / Design 6.2 */
static void test_dc_meter_tables(void)
{
    const ems_cfg_device_t *dev = dc_device();
    TEST_ASSERT_NOT_NULL(dev);
    const ems_cfg_profile_t *p = &ems_config_build.profiles[dev->profile_index];
    const ems_cfg_map_t *m = &ems_config_build.maps[p->map_index];
    TEST_ASSERT_EQUAL_STRING("DC_METER_SIM", p->profile_id);
    TEST_ASSERT_EQUAL_INT(EMS_RATIO_METER_REPORTS_PRIMARY, p->ratio_handling);
    TEST_ASSERT_EQUAL_DOUBLE(3000.0, p->counter_max_rate_per_hour);      /* role power.active, Design 5.5 */
    TEST_ASSERT_EQUAL_STRING("MYEMS_SIM_DCMETER", m->map_id);
    TEST_ASSERT_EQUAL_UINT8(3u, m->group_count);
    TEST_ASSERT_EQUAL_UINT16(6u, m->mapping_count);
    /* measurements FC04 0..5, energy FC04 100..103, status FC03 200 */
    TEST_ASSERT_EQUAL_UINT8(4u, m->groups[0].function_code);
    TEST_ASSERT_EQUAL_UINT16(0u, m->groups[0].start_address);
    TEST_ASSERT_EQUAL_UINT16(6u, m->groups[0].count);
    TEST_ASSERT_EQUAL_UINT32(4000u, m->groups[0].interval_ms);
    TEST_ASSERT_EQUAL_UINT8(4u, m->groups[1].function_code);
    TEST_ASSERT_EQUAL_UINT16(100u, m->groups[1].start_address);
    TEST_ASSERT_EQUAL_UINT8(3u, m->groups[2].function_code);
    TEST_ASSERT_EQUAL_UINT16(200u, m->groups[2].start_address);
    const uint16_t ids[6] = { EMS_PARAM_DC_VOLTAGE, EMS_PARAM_DC_CURRENT, EMS_PARAM_DC_POWER,
                              EMS_PARAM_DC_ENERGY_IMPORT, EMS_PARAM_DC_ENERGY_EXPORT, EMS_PARAM_DEV_STATUS_CODE };
    const uint16_t raw_ids[6] = { 0x0201u, 0x0211u, 0x0221u, 0x0240u, 0x0241u, 0x0001u };
    for (uint16_t i = 0; i < 6u; i++) {
        TEST_ASSERT_EQUAL_HEX16(raw_ids[i], ids[i]);
        TEST_ASSERT_EQUAL_HEX16(ids[i], m->mappings[i].param_id);
        TEST_ASSERT_EQUAL_UINT8(0u, m->mappings[i].flags & (EMS_MAP_F_APPLY_CT | EMS_MAP_F_APPLY_PT));
    }
    TEST_ASSERT_EQUAL_INT(EMS_REG_FLOAT32, m->mappings[0].type);
    TEST_ASSERT_EQUAL_INT(EMS_ORDER_ABCD, m->mappings[0].order);
    TEST_ASSERT_EQUAL_INT(EMS_REG_INT32, m->mappings[1].type);
    TEST_ASSERT_EQUAL_INT(EMS_ORDER_CDAB, m->mappings[1].order);
    TEST_ASSERT_EQUAL_INT(EMS_REG_FLOAT32, m->mappings[2].type);
    TEST_ASSERT_EQUAL_INT(EMS_ORDER_CDAB, m->mappings[2].order);
    TEST_ASSERT_TRUE((m->mappings[3].flags & EMS_MAP_F_COUNTER) != 0u);
    TEST_ASSERT_TRUE((m->mappings[4].flags & EMS_MAP_F_COUNTER) != 0u);
}
#endif

void run_config_site_tests(void)
{
    RUN_TEST(test_CFG019_site_activates_in_a_sim_build_only);
    RUN_TEST(test_site_devices);
    RUN_TEST(test_plan_from_site_tables);
#if defined(EMS_TEST_SITE_DC) || defined(EMS_TEST_SITE_MIXED)
    RUN_TEST(test_dc_meter_tables);
#endif
}
