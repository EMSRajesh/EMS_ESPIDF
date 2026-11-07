/*
 * Generated tables for config/site/lab_decoder_test.site.json: the decoder
 * vectors D1-D11 of Test Spec 4.1, through the generated mapping table.
 * Registers 1000..1019 hold the words the simulator will serve in D4.
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

static const uint16_t k_regs[20] = {
    0x4366, 0x8000,   /* 1000 D1  FLOAT32 ABCD 230.5 */
    0x8000, 0x4366,   /* 1002 D2  FLOAT32 CDAB 230.5 */
    0x6643, 0x0080,   /* 1004 D3  FLOAT32 BADC 230.5 */
    0x0080, 0x6643,   /* 1006 D4  FLOAT32 DCBA 230.5 */
    0xFFFF, 0xCF2C,   /* 1008 D5  INT32 ABCD -12500  */
    0xCF2C, 0xFFFF,   /* 1010 D6  INT32 CDAB -12500  */
    0x2729, 0x0000,   /* 1012 D7  UINT32 CDAB 10025  */
    0x7FC0, 0x0000,   /* 1014 D8  FLOAT32 NaN        */
    0x7F80, 0x0000,   /* 1016 D9  FLOAT32 +Inf       */
    0x8000,           /* 1018 D10 INT16 -32768       */
    0xFFFF            /* 1019 D11 UINT16 65535       */
};

static ems_decode_status_t conv_id(uint16_t mapping_id, double *value)
{
    const ems_cfg_map_t *m = &ems_config_build.maps[0];
    for (size_t i = 0; i < m->mapping_count; i++) {
        if (m->mappings[i].mapping_id == mapping_id) {
            const ems_poll_group_t *g = &m->groups[m->mappings[i].group_index];
            ems_regmap_result_t out = {0};
            ems_decode_status_t st = ems_regmap_convert(&m->mappings[i], g, k_regs, g->count, NULL, &out);
            *value = out.value;
            return st;
        }
    }
    TEST_FAIL_MESSAGE("mapping_id not in generated map");
    return EMS_DEC_INVALID;
}

static void test_DEC_vectors_D1_to_D11_through_generated_map(void)
{
    double v = 0.0;
    TEST_ASSERT_EQUAL_STRING("LAB_DECODER_TEST", ems_config_build.site_id);
    TEST_ASSERT_EQUAL_UINT8(EMS_CLASS_TEST_ONLY, ems_config_build.classification);
    TEST_ASSERT_EQUAL_UINT8(EMS_RATIO_METER_REPORTS_PRIMARY, ems_config_build.profiles[0].ratio_handling);
    TEST_ASSERT_EQUAL_UINT16(1000u, ems_config_build.maps[0].groups[0].start_address);
    TEST_ASSERT_EQUAL_UINT16(20u, ems_config_build.maps[0].groups[0].count);

    for (uint16_t id = 1u; id <= 4u; id++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(EMS_DEC_OK, conv_id(id, &v), "D1-D4 float orders");
        ASSERT_SAME_DOUBLE(230.5, v);
    }
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv_id(5u, &v));  ASSERT_SAME_DOUBLE(-12500.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv_id(6u, &v));  ASSERT_SAME_DOUBLE(-12500.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv_id(7u, &v));  ASSERT_SAME_DOUBLE(10025.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, conv_id(8u, &v));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, conv_id(9u, &v));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv_id(10u, &v)); ASSERT_SAME_DOUBLE(-32768.0, v);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv_id(11u, &v)); ASSERT_SAME_DOUBLE(65535.0, v);
}

void run_config_decoder_tests(void)
{
    RUN_TEST(test_DEC_vectors_D1_to_D11_through_generated_map);
}
