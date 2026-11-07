/*
 * Generated tables for config/site/lab_bench.site.json, end to end:
 * register words in, engineering values out, through the generated mapping
 * table and the D2 conversion engine (Design 8.2, Test Spec 4.1/4.3, CONV-004/006).
 */
#include <string.h>
#include "unity.h"
#include "ems_config_gen.h"
#include "ems_param_ids.h"
#include "ems_register_map.h"

static uint64_t dbits(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}
#define ASSERT_SAME_DOUBLE(expected, actual) TEST_ASSERT_EQUAL_HEX64(dbits(expected), dbits(actual))

static const ems_cfg_map_t *map0(void) { return &ems_config_build.maps[0]; }

static const ems_reg_mapping_t *find(uint16_t param_id)
{
    for (size_t i = 0; i < map0()->mapping_count; i++) {
        if (map0()->mappings[i].param_id == param_id) {
            return &map0()->mappings[i];
        }
    }
    TEST_FAIL_MESSAGE("parameter not in generated map");
    return NULL;
}

/* Simulated register space 0..18: Test Spec 4.1 vectors (import case). */
static uint16_t s_regs[19];

static void fill_nominal(void)
{
    static const uint16_t v[19] = {
        0x08FC, 0x0906, 0x08F7,     /* 0-2   2300 2310 2295 -> V x0.1   */
        0x03E8, 0x03F2, 0x03DE,     /* 3-5   1000 1010 990  -> A x0.01  */
        0x0000, 0x30D4,             /* 6-7   12500 -> 12.500 kW         */
        0x0000, 0x0BB8,             /* 8-9   3000  -> 3.000 kvar        */
        0x0000, 0x3237,             /* 10-11 12855 -> 12.855 kVA        */
        0x03CC,                     /* 12    972   -> 0.972             */
        0x1388,                     /* 13    5000  -> 50.00 Hz          */
        0x0000, 0x2729,             /* 14-15 10025 -> 100.25 kWh        */
        0x0000, 0x0000,             /* 16-17 0     -> 0.00 kWh          */
        0x0000                      /* 18    status 0                   */
    };
    memcpy(s_regs, v, sizeof(v));
}

static ems_decode_status_t conv(uint16_t param_id, const ems_ratio_t *r, double *value)
{
    const ems_reg_mapping_t *m = find(param_id);
    const ems_poll_group_t *g = &map0()->groups[m->group_index];
    ems_regmap_result_t out = {0};
    ems_decode_status_t st = ems_regmap_convert(m, g, &s_regs[g->start_address], g->count, r, &out);
    *value = out.value;
    return st;
}

static void test_E2E_lab_bench_layout(void)
{
    TEST_ASSERT_EQUAL_STRING("LAB_BENCH_01", ems_config_build.site_id);
    TEST_ASSERT_EQUAL_UINT8(1u, ems_config_build.device_count);
    TEST_ASSERT_EQUAL_UINT8(EMS_CLASS_SIMULATION_ONLY, ems_config_build.classification);
    TEST_ASSERT_EQUAL_UINT8(EMS_TARGET_SIM, ems_config_build.target);
    TEST_ASSERT_EQUAL_UINT8(3u, map0()->group_count);
    TEST_ASSERT_EQUAL_UINT16(14u, map0()->mapping_count);
    const ems_cfg_device_t *d = &ems_config_build.devices[0];
    TEST_ASSERT_EQUAL_UINT16(500u, d->timeout_ms);       /* v1.1 defaults */
    TEST_ASSERT_EQUAL_UINT8(1u, d->max_retries);
    TEST_ASSERT_EQUAL_UINT8(2u, d->comm_lost_threshold);
    TEST_ASSERT_EQUAL_UINT8(2u, d->probe_group);         /* "status" */
    TEST_ASSERT_EQUAL_UINT32(30000u, d->probe_interval_ms);
    const ems_cfg_bus_t *b = &ems_config_build.buses[0];
    TEST_ASSERT_EQUAL_UINT32(9600u, b->baud);
    TEST_ASSERT_EQUAL_UINT8(EMS_PARITY_EVEN, b->parity);
    TEST_ASSERT_EQUAL_UINT8(17u, b->tx_gpio);
    TEST_ASSERT_EQUAL_UINT8(18u, b->rx_gpio);
    TEST_ASSERT_EQUAL_UINT8(21u, b->rts_gpio);
    ASSERT_SAME_DOUBLE(1000.0, ems_config_build.profiles[0].counter_max_rate_per_hour);
}

/* Test Spec 4.1 through the generated table (CT = PT = 1). */
static void test_E2E_vectors_4_1_through_generated_map(void)
{
    const ems_ratio_t r1 = {1.0, 1.0};
    double v = 0.0;
    fill_nominal();
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_VOLTAGE_L1_N, &r1, &v));        ASSERT_SAME_DOUBLE(230.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_VOLTAGE_L2_N, &r1, &v));        ASSERT_SAME_DOUBLE(231.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_VOLTAGE_L3_N, &r1, &v));        ASSERT_SAME_DOUBLE(229.5, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_CURRENT_L1, &r1, &v));          ASSERT_SAME_DOUBLE(10.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_CURRENT_L2, &r1, &v));          ASSERT_SAME_DOUBLE(10.1, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_CURRENT_L3, &r1, &v));          ASSERT_SAME_DOUBLE(9.9, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_POWER_ACTIVE_TOTAL, &r1, &v));  ASSERT_SAME_DOUBLE(12.5, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_POWER_REACTIVE_TOTAL, &r1, &v)); ASSERT_SAME_DOUBLE(3.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_POWER_APPARENT_TOTAL, &r1, &v)); ASSERT_SAME_DOUBLE(12.855, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_POWER_FACTOR_TOTAL, &r1, &v));  ASSERT_SAME_DOUBLE(0.972, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_FREQUENCY, &r1, &v));           ASSERT_SAME_DOUBLE(50.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, &r1, &v)); ASSERT_SAME_DOUBLE(100.25, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE, &r1, &v)); ASSERT_SAME_DOUBLE(0.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_DEV_STATUS_CODE, &r1, &v));        ASSERT_SAME_DOUBLE(0.0, v);
    /* export case */
    s_regs[6] = 0xFFFF; s_regs[7] = 0xCF2C; s_regs[12] = 0xFC34;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_POWER_ACTIVE_TOTAL, &r1, &v));  ASSERT_SAME_DOUBLE(-12.5, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_POWER_FACTOR_TOTAL, &r1, &v));  ASSERT_SAME_DOUBLE(-0.972, v);
}

/* Test Spec 4.3 (site lab_ratio_ct200: CT 200, PT 1): ratios follow the
 * generated flags; min/max apply to the value after the ratio. */
static void test_E2E_vectors_4_3_ct200(void)
{
    const ems_ratio_t ct200 = {200.0, 1.0};
    double v = 0.0;
    fill_nominal();
    s_regs[3] = 250u;                       /* current l1 */
    s_regs[4] = 600u;                       /* current l2 */
    s_regs[6] = 0x0000; s_regs[7] = 1000u;  /* active power */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_CURRENT_L1, &ct200, &v));        ASSERT_SAME_DOUBLE(500.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OUT_OF_RANGE, conv(EMS_PARAM_AC_CURRENT_L2, &ct200, &v)); ASSERT_SAME_DOUBLE(1200.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_VOLTAGE_L1_N, &ct200, &v));      ASSERT_SAME_DOUBLE(230.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_POWER_ACTIVE_TOTAL, &ct200, &v)); ASSERT_SAME_DOUBLE(200.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, &ct200, &v)); ASSERT_SAME_DOUBLE(20050.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_FREQUENCY, &ct200, &v));         ASSERT_SAME_DOUBLE(50.0, v);
}

static void test_E2E_out_of_range_and_short_response(void)
{
    const ems_ratio_t r1 = {1.0, 1.0};
    double v = 0.0;
    fill_nominal();
    s_regs[13] = 7000;   /* 70 Hz > 65 Hz */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OUT_OF_RANGE, conv(EMS_PARAM_AC_FREQUENCY, &r1, &v));
    const ems_poll_group_t *g = &map0()->groups[0];
    TEST_ASSERT_TRUE(ems_regmap_response_complete(g, 14u));
    TEST_ASSERT_FALSE(ems_regmap_response_complete(g, 13u));
}

void run_config_lab_tests(void)
{
    RUN_TEST(test_E2E_lab_bench_layout);
    RUN_TEST(test_E2E_vectors_4_1_through_generated_map);
    RUN_TEST(test_E2E_vectors_4_3_ct200);
    RUN_TEST(test_E2E_out_of_range_and_short_response);
}
