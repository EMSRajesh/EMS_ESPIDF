/*
 * Per-site generated tables (DP-WP4 D9): main/generated_sites/<site>/.
 * Built once per site (see CMakeLists.txt); EMS_TEST_SITE_2DEV or
 * EMS_TEST_SITE_CT200 selects the expectations. The common table checks
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
#else
#error "define EMS_TEST_SITE_2DEV or EMS_TEST_SITE_CT200"
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

void run_config_site_tests(void)
{
    RUN_TEST(test_CFG019_site_activates_in_a_sim_build_only);
    RUN_TEST(test_site_devices);
    RUN_TEST(test_plan_from_site_tables);
}
