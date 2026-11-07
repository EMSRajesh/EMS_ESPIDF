/*
 * Register map engine tests (DP-WP4 4C).
 * DEC-001..010, CONV-001..008 and map validation. Vectors from the
 * DP-WP4 Verification & Test Specification v1.1, Section 4.
 */
#include <math.h>
#include <string.h>

#include "unity.h"
#include "ems_register_map.h"
#include "ems_param_ids.h"

/* ------------------------------------------------------------- fixtures --- */

/* Design 8.2: AC meter poll groups. */
static const ems_poll_group_t k_groups[] = {
    { 5000u, 500u, 11000u, 0u, 14u, 3u },   /* 0 electrical: 0-13  */
    { 10000u, 1500u, 21000u, 14u, 4u, 3u }, /* 1 energy:     14-17 */
    { 30000u, 2500u, 61000u, 18u, 1u, 3u }, /* 2 status:     18    */
};

#define CT_PT (EMS_MAP_F_APPLY_CT | EMS_MAP_F_APPLY_PT)
#define CNT   (EMS_MAP_F_COUNTER)

/* Design 8.2: AC meter mappings (scale, offset, min, max, id, param, addr, mask, group, type, order, flags). */
static const ems_reg_mapping_t k_maps[] = {
    { 0.1,   0.0, 0.0,     500.0,   1, EMS_PARAM_AC_VOLTAGE_L1_N,         0,  0, 0, EMS_REG_UINT16, EMS_ORDER_AB,   EMS_MAP_F_APPLY_PT },
    { 0.1,   0.0, 0.0,     500.0,   2, EMS_PARAM_AC_VOLTAGE_L2_N,         1,  0, 0, EMS_REG_UINT16, EMS_ORDER_AB,   EMS_MAP_F_APPLY_PT },
    { 0.1,   0.0, 0.0,     500.0,   3, EMS_PARAM_AC_VOLTAGE_L3_N,         2,  0, 0, EMS_REG_UINT16, EMS_ORDER_AB,   EMS_MAP_F_APPLY_PT },
    { 0.01,  0.0, 0.0,     650.0,   4, EMS_PARAM_AC_CURRENT_L1,           3,  0, 0, EMS_REG_UINT16, EMS_ORDER_AB,   EMS_MAP_F_APPLY_CT },
    { 0.01,  0.0, 0.0,     650.0,   5, EMS_PARAM_AC_CURRENT_L2,           4,  0, 0, EMS_REG_UINT16, EMS_ORDER_AB,   EMS_MAP_F_APPLY_CT },
    { 0.01,  0.0, 0.0,     650.0,   6, EMS_PARAM_AC_CURRENT_L3,           5,  0, 0, EMS_REG_UINT16, EMS_ORDER_AB,   EMS_MAP_F_APPLY_CT },
    { 0.001, 0.0, -1000.0, 1000.0,  7, EMS_PARAM_AC_POWER_ACTIVE_TOTAL,   6,  0, 0, EMS_REG_INT32,  EMS_ORDER_ABCD, CT_PT },
    { 0.001, 0.0, -1000.0, 1000.0,  8, EMS_PARAM_AC_POWER_REACTIVE_TOTAL, 8,  0, 0, EMS_REG_INT32,  EMS_ORDER_ABCD, CT_PT },
    { 0.001, 0.0, 0.0,     1000.0,  9, EMS_PARAM_AC_POWER_APPARENT_TOTAL, 10, 0, 0, EMS_REG_UINT32, EMS_ORDER_ABCD, CT_PT },
    { 0.001, 0.0, -1.0,    1.0,    10, EMS_PARAM_AC_POWER_FACTOR_TOTAL,   12, 0, 0, EMS_REG_INT16,  EMS_ORDER_AB,   0 },
    { 0.01,  0.0, 45.0,    65.0,   11, EMS_PARAM_AC_FREQUENCY,            13, 0, 0, EMS_REG_UINT16, EMS_ORDER_AB,   0 },
    { 0.01,  0.0, 0.0,     4.3e7,  12, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, 14, 0, 1, EMS_REG_UINT32, EMS_ORDER_ABCD, CT_PT | CNT },
    { 0.01,  0.0, 0.0,     4.3e7,  13, EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE, 16, 0, 1, EMS_REG_UINT32, EMS_ORDER_ABCD, CT_PT | CNT },
    { 1.0,   0.0, 0.0,     65535.0,14, EMS_PARAM_DEV_STATUS_CODE,         18, 0, 2, EMS_REG_UINT16, EMS_ORDER_AB,   0 },
};
#define N_MAPS (sizeof(k_maps) / sizeof(k_maps[0]))
#define N_GROUPS (sizeof(k_groups) / sizeof(k_groups[0]))

/* Test Spec 4.1: register image, import case. */
static const uint16_t k_electrical[14] = {
    0x08FC, 0x0906, 0x08F7,           /* 230.0 231.0 229.5 V   */
    0x03E8, 0x03F2, 0x03DE,           /* 10.00 10.10 9.90 A    */
    0x0000, 0x30D4,                   /* +12.500 kW            */
    0x0000, 0x0BB8,                   /* 3.000 kvar            */
    0x0000, 0x3237,                   /* 12.855 kVA            */
    0x03CC,                           /* 0.972                 */
    0x1388,                           /* 50.00 Hz              */
};
static const uint16_t k_energy[4] = { 0x0000, 0x2729, 0x0000, 0x0000 };  /* 100.25 / 0.00 kWh */
static const uint16_t k_status[1] = { 0x0000 };

static const ems_reg_mapping_t *map_for(uint16_t param)
{
    for (size_t i = 0; i < N_MAPS; ++i) {
        if (k_maps[i].param_id == param) {
            return &k_maps[i];
        }
    }
    TEST_FAIL_MESSAGE("mapping not found");
    return NULL;
}

static const uint16_t *image_for(const ems_reg_mapping_t *m, uint16_t *count)
{
    switch (m->group_index) {
    case 0: *count = 14; return k_electrical;
    case 1: *count = 4;  return k_energy;
    default: *count = 1; return k_status;
    }
}

/* Convert one AC meter parameter from the default image. */
static ems_decode_status_t conv(uint16_t param, const ems_ratio_t *ratio, double *value)
{
    const ems_reg_mapping_t *m = map_for(param);
    uint16_t n = 0;
    const uint16_t *regs = image_for(m, &n);
    ems_regmap_result_t r;
    const ems_decode_status_t st = ems_regmap_convert(m, &k_groups[m->group_index], regs, n, ratio, &r);
    *value = r.value;
    return st;
}

/* Decode one mapping of a given type/order against two words at address 0. */
static ems_decode_status_t dec2(uint8_t type, uint8_t order, uint16_t r0, uint16_t r1,
                                ems_regmap_result_t *out)
{
    static const ems_poll_group_t g = { 1000u, 0u, 2000u, 0u, 2u, 3u };
    const uint16_t regs[2] = { r0, r1 };
    ems_reg_mapping_t m;
    memset(&m, 0, sizeof(m));
    m.scale = 1.0; m.min = -1e12; m.max = 1e12;
    m.type = type; m.order = order;
    return ems_regmap_decode(&m, &g, regs, 2u, out);
}

#define TOL 1e-9

/*
 * Bit-exact double comparison. Do not write "value == 2.3": compilers using
 * x87 (32-bit MinGW, FLT_EVAL_METHOD 2) evaluate the constant in 80-bit
 * precision, so the comparison fails even when value is the correctly rounded
 * double. Passing both through a double parameter rounds the constant first.
 */
static uint64_t dbits(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}
#define ASSERT_SAME_DOUBLE(expected, actual) \
    TEST_ASSERT_EQUAL_HEX64(dbits(expected), dbits(actual))

/* ------------------------------------------------------------ DEC tests --- */

static void test_DEC001_uint16(void)
{
    ems_regmap_result_t r;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, dec2(EMS_REG_UINT16, EMS_ORDER_AB, 0x08FC, 0, &r));
    TEST_ASSERT_EQUAL_DOUBLE(2300.0, r.value);
    dec2(EMS_REG_UINT16, EMS_ORDER_AB, 0xFFFF, 0, &r);        /* D11 */
    TEST_ASSERT_EQUAL_DOUBLE(65535.0, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, dec2(EMS_REG_UINT16, EMS_ORDER_BA, 0xFC08, 0, &r));
    TEST_ASSERT_EQUAL_DOUBLE(2300.0, r.value);                /* byte-swapped 16-bit */
}

static void test_DEC002_int16_positive_and_negative(void)
{
    ems_regmap_result_t r;
    dec2(EMS_REG_INT16, EMS_ORDER_AB, 0x03CC, 0, &r);
    TEST_ASSERT_EQUAL_DOUBLE(972.0, r.value);
    dec2(EMS_REG_INT16, EMS_ORDER_AB, 0xFC34, 0, &r);
    TEST_ASSERT_EQUAL_DOUBLE(-972.0, r.value);
    TEST_ASSERT_EQUAL_INT64(-972, r.raw);
    dec2(EMS_REG_INT16, EMS_ORDER_AB, 0x8000, 0, &r);        /* D10 */
    TEST_ASSERT_EQUAL_DOUBLE(-32768.0, r.value);
    dec2(EMS_REG_INT16, EMS_ORDER_AB, 0x7FFF, 0, &r);
    TEST_ASSERT_EQUAL_DOUBLE(32767.0, r.value);
}

static void test_DEC003_int32_abcd_negative(void)
{
    ems_regmap_result_t r;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, dec2(EMS_REG_INT32, EMS_ORDER_ABCD, 0xFFFF, 0xCF2C, &r)); /* D5 */
    TEST_ASSERT_EQUAL_DOUBLE(-12500.0, r.value);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFCF2Cu, r.raw_bits);
    dec2(EMS_REG_INT32, EMS_ORDER_ABCD, 0x8000, 0x0000, &r);
    TEST_ASSERT_EQUAL_DOUBLE(-2147483648.0, r.value);
    dec2(EMS_REG_INT32, EMS_ORDER_ABCD, 0x7FFF, 0xFFFF, &r);
    TEST_ASSERT_EQUAL_DOUBLE(2147483647.0, r.value);
}

static void test_DEC004_int32_cdab(void)
{
    ems_regmap_result_t r;
    dec2(EMS_REG_INT32, EMS_ORDER_CDAB, 0xCF2C, 0xFFFF, &r);  /* D6 */
    TEST_ASSERT_EQUAL_DOUBLE(-12500.0, r.value);
}

static void test_DEC005_uint32_abcd_and_cdab(void)
{
    ems_regmap_result_t r;
    dec2(EMS_REG_UINT32, EMS_ORDER_ABCD, 0x0000, 0x2729, &r);
    TEST_ASSERT_EQUAL_DOUBLE(10025.0, r.value);
    dec2(EMS_REG_UINT32, EMS_ORDER_CDAB, 0x2729, 0x0000, &r);  /* D7 */
    TEST_ASSERT_EQUAL_DOUBLE(10025.0, r.value);
    dec2(EMS_REG_UINT32, EMS_ORDER_ABCD, 0xFFFF, 0xFFFF, &r);
    TEST_ASSERT_EQUAL_DOUBLE(4294967295.0, r.value);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, r.raw_bits);
}

static void test_DEC006_float32_all_four_orders(void)
{
    ems_regmap_result_t r;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, dec2(EMS_REG_FLOAT32, EMS_ORDER_ABCD, 0x4366, 0x8000, &r)); /* D1 */
    TEST_ASSERT_EQUAL_DOUBLE(230.5, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, dec2(EMS_REG_FLOAT32, EMS_ORDER_CDAB, 0x8000, 0x4366, &r)); /* D2 */
    TEST_ASSERT_EQUAL_DOUBLE(230.5, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, dec2(EMS_REG_FLOAT32, EMS_ORDER_BADC, 0x6643, 0x0080, &r)); /* D3 */
    TEST_ASSERT_EQUAL_DOUBLE(230.5, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, dec2(EMS_REG_FLOAT32, EMS_ORDER_DCBA, 0x0080, 0x6643, &r)); /* D4 */
    TEST_ASSERT_EQUAL_DOUBLE(230.5, r.value);
    /* The 32-bit integer types use the same assembly. */
    dec2(EMS_REG_UINT32, EMS_ORDER_BADC, 0x6643, 0x0080, &r);
    TEST_ASSERT_EQUAL_HEX32(0x43668000u, r.raw_bits);
    dec2(EMS_REG_UINT32, EMS_ORDER_DCBA, 0x0080, 0x6643, &r);
    TEST_ASSERT_EQUAL_HEX32(0x43668000u, r.raw_bits);
}

static void test_DEC007_float32_nan_and_inf_are_invalid(void)
{
    ems_regmap_result_t r;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, dec2(EMS_REG_FLOAT32, EMS_ORDER_ABCD, 0x7FC0, 0x0000, &r)); /* D8 */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, dec2(EMS_REG_FLOAT32, EMS_ORDER_ABCD, 0x7F80, 0x0000, &r)); /* D9 */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, dec2(EMS_REG_FLOAT32, EMS_ORDER_ABCD, 0xFF80, 0x0000, &r)); /* -Inf */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, dec2(EMS_REG_FLOAT32, EMS_ORDER_ABCD, 0x7F80, 0x0001, &r)); /* signalling NaN */
    /* Largest finite float is valid. */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, dec2(EMS_REG_FLOAT32, EMS_ORDER_ABCD, 0x7F7F, 0xFFFF, &r));
}

static void test_DEC008_bitfield_mask_and_shift(void)
{
    static const ems_poll_group_t g = { 1000u, 0u, 2000u, 0u, 1u, 3u };
    const uint16_t regs[1] = { 0x1234 };
    ems_reg_mapping_t m;
    memset(&m, 0, sizeof(m));
    m.scale = 1.0; m.max = 65535.0; m.type = EMS_REG_BITFIELD16;
    ems_regmap_result_t r;

    m.bit_mask = 0x00F0;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_decode(&m, &g, regs, 1u, &r));
    TEST_ASSERT_EQUAL_DOUBLE(3.0, r.value);
    m.bit_mask = 0x8000;
    ems_regmap_decode(&m, &g, regs, 1u, &r);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.value);
    m.bit_mask = 0x0004;
    ems_regmap_decode(&m, &g, regs, 1u, &r);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, r.value);
    m.bit_mask = 0;                                           /* whole word */
    ems_regmap_decode(&m, &g, regs, 1u, &r);
    TEST_ASSERT_EQUAL_DOUBLE(4660.0, r.value);
    TEST_ASSERT_EQUAL_HEX32(0x1234u, r.raw_bits);
}

static void test_DEC009_short_response_is_rejected_for_the_whole_group(void)
{
    const ems_poll_group_t *g = &k_groups[0];
    TEST_ASSERT_TRUE(ems_regmap_response_complete(g, 14));
    TEST_ASSERT_FALSE(ems_regmap_response_complete(g, 13));
    TEST_ASSERT_FALSE(ems_regmap_response_complete(g, 15));
    TEST_ASSERT_FALSE(ems_regmap_response_complete(g, 0));
    TEST_ASSERT_FALSE(ems_regmap_response_complete(NULL, 14));

    /* Even if called anyway, a mapping beyond the received registers is INVALID. */
    ems_regmap_result_t r;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID,
        ems_regmap_convert(map_for(EMS_PARAM_AC_FREQUENCY), g, k_electrical, 13, NULL, &r));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID,                        /* INT32 needs regs 6 and 7 */
        ems_regmap_convert(map_for(EMS_PARAM_AC_POWER_ACTIVE_TOTAL), g, k_electrical, 7, NULL, &r));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK,
        ems_regmap_convert(map_for(EMS_PARAM_AC_VOLTAGE_L1_N), g, k_electrical, 13, NULL, &r));
}

static void test_DEC010_last_registers_of_a_125_register_group(void)
{
    static const ems_poll_group_t g = { 1000u, 0u, 2000u, 1000u, 125u, 4u };
    uint16_t regs[125];
    for (uint16_t i = 0; i < 125u; ++i) {
        regs[i] = i;
    }
    ems_reg_mapping_t m;
    memset(&m, 0, sizeof(m));
    m.scale = 1.0; m.min = 0.0; m.max = 1e12;
    m.type = EMS_REG_UINT32; m.order = EMS_ORDER_ABCD;
    ems_regmap_result_t r;

    m.address = 1123;                                         /* regs 123,124: last pair */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_decode(&m, &g, regs, 125u, &r));
    TEST_ASSERT_EQUAL_HEX32((123u << 16) | 124u, r.raw_bits);
    m.address = 1124;                                         /* would need reg 125 */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, ems_regmap_decode(&m, &g, regs, 125u, &r));
    m.address = 999;                                          /* before the block */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, ems_regmap_decode(&m, &g, regs, 125u, &r));
    m.type = EMS_REG_UINT16; m.address = 1124;                /* last single register */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_decode(&m, &g, regs, 125u, &r));
    TEST_ASSERT_EQUAL_DOUBLE(124.0, r.value);
}

static void test_decode_rejects_bad_type_order_and_null(void)
{
    ems_regmap_result_t r;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, dec2(EMS_REG_TYPE_COUNT, EMS_ORDER_ABCD, 1, 2, &r));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, dec2(EMS_REG_UINT16, EMS_ORDER_CDAB, 1, 2, &r));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, dec2(EMS_REG_INT16, EMS_ORDER_DCBA, 1, 2, &r));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, dec2(EMS_REG_UINT32, EMS_ORDER_COUNT, 1, 2, &r));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, ems_regmap_decode(NULL, &k_groups[0], k_electrical, 14, &r));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, ems_regmap_decode(&k_maps[0], NULL, k_electrical, 14, &r));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, ems_regmap_decode(&k_maps[0], &k_groups[0], NULL, 14, &r));
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, ems_regmap_decode(&k_maps[0], &k_groups[0], k_electrical, 14, NULL));
    TEST_ASSERT_EQUAL_UINT8(0u, ems_regmap_type_regs(EMS_REG_TYPE_COUNT));
}

/* ----------------------------------------------------------- CONV tests --- */

static void test_CONV_ac_meter_vectors_test_spec_4_1(void)
{
    struct { uint16_t p; double v; } exp[] = {
        { EMS_PARAM_AC_VOLTAGE_L1_N, 230.0 }, { EMS_PARAM_AC_VOLTAGE_L2_N, 231.0 },
        { EMS_PARAM_AC_VOLTAGE_L3_N, 229.5 }, { EMS_PARAM_AC_CURRENT_L1, 10.00 },
        { EMS_PARAM_AC_CURRENT_L2, 10.10 },   { EMS_PARAM_AC_CURRENT_L3, 9.90 },
        { EMS_PARAM_AC_POWER_ACTIVE_TOTAL, 12.500 }, { EMS_PARAM_AC_POWER_REACTIVE_TOTAL, 3.000 },
        { EMS_PARAM_AC_POWER_APPARENT_TOTAL, 12.855 }, { EMS_PARAM_AC_POWER_FACTOR_TOTAL, 0.972 },
        { EMS_PARAM_AC_FREQUENCY, 50.00 }, { EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, 100.25 },
        { EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE, 0.00 }, { EMS_PARAM_DEV_STATUS_CODE, 0.0 },
    };
    const ems_ratio_t unity_ratio = { 1.0, 1.0 };
    for (size_t i = 0; i < sizeof(exp) / sizeof(exp[0]); ++i) {
        double v = -1.0;
        TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(exp[i].p, &unity_ratio, &v));
        TEST_ASSERT_DOUBLE_WITHIN(TOL, exp[i].v, v);
    }
}

static void test_CONV_decimal_scales_are_exact(void)
{
    double v;
    conv(EMS_PARAM_AC_VOLTAGE_L1_N, NULL, &v);
    ASSERT_SAME_DOUBLE(230.0, v);
    conv(EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, NULL, &v);
    ASSERT_SAME_DOUBLE(100.25, v);
    conv(EMS_PARAM_AC_VOLTAGE_L3_N, NULL, &v);
    ASSERT_SAME_DOUBLE(229.5, v);

    /* Where multiplication is NOT exact: 2300 * 0.001 = 2.3000000000000003,
     * 2300 / 1000 = 2.3 (the nearest double to the decimal value). */
    static const ems_poll_group_t g = { 1000u, 0u, 2000u, 0u, 1u, 3u };
    const uint16_t regs[1] = { 2300 };
    ems_reg_mapping_t m;
    memset(&m, 0, sizeof(m));
    m.scale = 0.001; m.min = 0.0; m.max = 100.0; m.type = EMS_REG_UINT16;
    ems_regmap_result_t r;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_convert(&m, &g, regs, 1, NULL, &r));
    ASSERT_SAME_DOUBLE(2.3, r.value);
}

static void test_CONV001_scale_and_offset(void)
{
    static const ems_poll_group_t g = { 1000u, 0u, 2000u, 0u, 1u, 3u };
    const uint16_t regs[1] = { 100 };
    ems_reg_mapping_t m;
    memset(&m, 0, sizeof(m));
    m.min = -1e6; m.max = 1e6; m.type = EMS_REG_UINT16;
    ems_regmap_result_t r;

    m.scale = 0.5; m.offset = -10.0;                      /* 100 * 0.5 - 10 */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_convert(&m, &g, regs, 1, NULL, &r));
    TEST_ASSERT_EQUAL_DOUBLE(40.0, r.value);
    m.scale = 10.0; m.offset = 0.0;
    ems_regmap_convert(&m, &g, regs, 1, NULL, &r);
    TEST_ASSERT_EQUAL_DOUBLE(1000.0, r.value);
    m.scale = -0.1;                                       /* negative scale */
    ems_regmap_convert(&m, &g, regs, 1, NULL, &r);
    TEST_ASSERT_DOUBLE_WITHIN(TOL, -10.0, r.value);
    m.scale = 0.3;                                        /* 1/0.3 not whole: multiply */
    ems_regmap_convert(&m, &g, regs, 1, NULL, &r);
    TEST_ASSERT_DOUBLE_WITHIN(TOL, 30.0, r.value);
    m.scale = 1.0; m.offset = -273.15;
    ems_regmap_convert(&m, &g, regs, 1, NULL, &r);
    TEST_ASSERT_DOUBLE_WITHIN(TOL, -173.15, r.value);
}

static void test_CONV002_range_limits_are_inclusive(void)
{
    static const ems_poll_group_t g = { 1000u, 0u, 2000u, 0u, 1u, 3u };
    ems_reg_mapping_t m;
    memset(&m, 0, sizeof(m));
    m.scale = 0.01; m.min = 45.0; m.max = 65.0; m.type = EMS_REG_UINT16;
    ems_regmap_result_t r;
    uint16_t lo = 4500, hi = 6500;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_convert(&m, &g, &lo, 1, NULL, &r));
    TEST_ASSERT_EQUAL_DOUBLE(45.0, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_convert(&m, &g, &hi, 1, NULL, &r));
    TEST_ASSERT_EQUAL_DOUBLE(65.0, r.value);
}

static void test_CONV003_outside_range_is_out_of_range_with_value(void)
{
    static const ems_poll_group_t g = { 1000u, 0u, 2000u, 0u, 1u, 3u };
    ems_reg_mapping_t m;
    memset(&m, 0, sizeof(m));
    m.scale = 0.01; m.min = 45.0; m.max = 65.0; m.type = EMS_REG_UINT16;
    ems_regmap_result_t r;
    uint16_t below = 4499, above = 6501, sim_out_of_range = 3000;  /* SIM-009: 30.00 Hz */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OUT_OF_RANGE, ems_regmap_convert(&m, &g, &below, 1, NULL, &r));
    TEST_ASSERT_DOUBLE_WITHIN(TOL, 44.99, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OUT_OF_RANGE, ems_regmap_convert(&m, &g, &above, 1, NULL, &r));
    TEST_ASSERT_DOUBLE_WITHIN(TOL, 65.01, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OUT_OF_RANGE, ems_regmap_convert(&m, &g, &sim_out_of_range, 1, NULL, &r));
    TEST_ASSERT_EQUAL_DOUBLE(30.0, r.value);
}

static void test_CONV004_ratio_vectors_test_spec_4_3(void)
{
    /* Site lab_ratio_ct200: CT 200, PT 1, MYEMS_APPLIES. */
    const ems_ratio_t ratio = { 200.0, 1.0 };
    static const ems_poll_group_t *ge = &k_groups[0];
    ems_regmap_result_t r;
    uint16_t img[14];
    memcpy(img, k_electrical, sizeof(img));
    img[3] = 250;      /* I1 2.50 A secondary  */
    img[4] = 600;      /* I2 6.00 A secondary  */
    img[6] = 0; img[7] = 1000;   /* P 1.000 kW secondary */

    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_convert(map_for(EMS_PARAM_AC_CURRENT_L1), ge, img, 14, &ratio, &r));
    TEST_ASSERT_EQUAL_DOUBLE(500.0, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OUT_OF_RANGE, ems_regmap_convert(map_for(EMS_PARAM_AC_CURRENT_L2), ge, img, 14, &ratio, &r));
    TEST_ASSERT_EQUAL_DOUBLE(1200.0, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_convert(map_for(EMS_PARAM_AC_VOLTAGE_L1_N), ge, img, 14, &ratio, &r));
    TEST_ASSERT_EQUAL_DOUBLE(230.0, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_convert(map_for(EMS_PARAM_AC_POWER_ACTIVE_TOTAL), ge, img, 14, &ratio, &r));
    TEST_ASSERT_EQUAL_DOUBLE(200.0, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_convert(map_for(EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE), &k_groups[1], k_energy, 4, &ratio, &r));
    TEST_ASSERT_EQUAL_DOUBLE(20050.0, r.value);
    /* Frequency and power factor never get a ratio. */
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_convert(map_for(EMS_PARAM_AC_FREQUENCY), ge, img, 14, &ratio, &r));
    TEST_ASSERT_EQUAL_DOUBLE(50.0, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, ems_regmap_convert(map_for(EMS_PARAM_AC_POWER_FACTOR_TOTAL), ge, img, 14, &ratio, &r));
    TEST_ASSERT_DOUBLE_WITHIN(TOL, 0.972, r.value);
}

static void test_CONV005_meter_reports_primary_means_no_ratio(void)
{
    double v;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK, conv(EMS_PARAM_AC_CURRENT_L1, NULL, &v));
    TEST_ASSERT_EQUAL_DOUBLE(10.0, v);
    TEST_ASSERT_EQUAL_DOUBLE(1.0, ems_regmap_ratio_factor(map_for(EMS_PARAM_AC_CURRENT_L1), NULL));
}

static void test_CONV006_ratio_is_applied_exactly_once(void)
{
    const ems_ratio_t ratio = { 200.0, 11.0 };   /* e.g. 11 kV PT ratio */
    TEST_ASSERT_EQUAL_DOUBLE(200.0,  ems_regmap_ratio_factor(map_for(EMS_PARAM_AC_CURRENT_L1), &ratio));
    TEST_ASSERT_EQUAL_DOUBLE(11.0,   ems_regmap_ratio_factor(map_for(EMS_PARAM_AC_VOLTAGE_L1_N), &ratio));
    TEST_ASSERT_EQUAL_DOUBLE(2200.0, ems_regmap_ratio_factor(map_for(EMS_PARAM_AC_POWER_ACTIVE_TOTAL), &ratio));
    TEST_ASSERT_EQUAL_DOUBLE(1.0,    ems_regmap_ratio_factor(map_for(EMS_PARAM_AC_FREQUENCY), &ratio));
    double v;
    conv(EMS_PARAM_AC_CURRENT_L1, &ratio, &v);
    TEST_ASSERT_EQUAL_DOUBLE(2000.0, v);          /* 10 A x 200, not x 200^2 */
}

static void test_CONV007_largest_energy_counter_is_exact(void)
{
    const uint16_t regs[4] = { 0xFFFF, 0xFFFF, 0x0000, 0x0000 };
    ems_regmap_result_t r;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK,
        ems_regmap_convert(map_for(EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE), &k_groups[1], regs, 4, NULL, &r));
    ASSERT_SAME_DOUBLE(42949672.95, r.value);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, r.raw_bits);
    /* One count below differs by exactly 0.01 kWh. */
    const uint16_t regs2[4] = { 0xFFFF, 0xFFFE, 0x0000, 0x0000 };
    double prev = r.value;
    ems_regmap_convert(map_for(EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE), &k_groups[1], regs2, 4, NULL, &r);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.01, prev - r.value);
}

static void test_CONV008_power_sign_and_pf_sign(void)
{
    uint16_t img[14];
    memcpy(img, k_electrical, sizeof(img));
    img[6] = 0xFFFF; img[7] = 0xCF2C;     /* -12500 -> -12.500 kW export */
    img[12] = 0xFC34;                     /* -0.972 */
    ems_regmap_result_t r;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK,
        ems_regmap_convert(map_for(EMS_PARAM_AC_POWER_ACTIVE_TOTAL), &k_groups[0], img, 14, NULL, &r));
    TEST_ASSERT_DOUBLE_WITHIN(TOL, -12.5, r.value);
    TEST_ASSERT_EQUAL_INT(EMS_DEC_OK,
        ems_regmap_convert(map_for(EMS_PARAM_AC_POWER_FACTOR_TOTAL), &k_groups[0], img, 14, NULL, &r));
    TEST_ASSERT_DOUBLE_WITHIN(TOL, -0.972, r.value);
    double v;
    conv(EMS_PARAM_AC_POWER_ACTIVE_TOTAL, NULL, &v);
    TEST_ASSERT_TRUE(v > 0.0);            /* import positive */
}

static void test_convert_non_finite_result_is_invalid(void)
{
    static const ems_poll_group_t g = { 1000u, 0u, 2000u, 0u, 2u, 3u };
    const uint16_t regs[2] = { 0x7F7F, 0xFFFF };   /* FLT_MAX */
    ems_reg_mapping_t m;
    memset(&m, 0, sizeof(m));
    m.scale = 1e300; m.min = -1e308; m.max = 1e308; m.type = EMS_REG_FLOAT32;
    ems_regmap_result_t r;
    TEST_ASSERT_EQUAL_INT(EMS_DEC_INVALID, ems_regmap_convert(&m, &g, regs, 2, NULL, &r));
}

/* ------------------------------------------------------ validation tests --- */

static ems_map_check_t validate_with(const ems_reg_mapping_t *one)
{
    return ems_regmap_validate(k_groups, N_GROUPS, one, 1, NULL);
}

static void test_validate_accepts_the_ac_meter_map(void)
{
    size_t bad = 99;
    TEST_ASSERT_EQUAL_INT(EMS_MAP_OK, ems_regmap_validate(k_groups, N_GROUPS, k_maps, N_MAPS, &bad));
}

static void test_validate_rejects_bad_groups(void)
{
    ems_poll_group_t g[1] = { { 5000u, 0u, 11000u, 0u, 14u, 3u } };
    size_t bad = 99;
    g[0].function_code = 6;    TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_GROUP_FC, ems_regmap_validate(g, 1, NULL, 0, &bad));
    TEST_ASSERT_EQUAL_UINT32(0u, bad);
    g[0].function_code = 4;    TEST_ASSERT_EQUAL_INT(EMS_MAP_OK, ems_regmap_validate(g, 1, NULL, 0, &bad));
    g[0].count = 0;            TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_GROUP_COUNT, ems_regmap_validate(g, 1, NULL, 0, &bad));
    g[0].count = 126;          TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_GROUP_COUNT, ems_regmap_validate(g, 1, NULL, 0, &bad));
    g[0].count = 10; g[0].start_address = 65530;
                               TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_GROUP_COUNT, ems_regmap_validate(g, 1, NULL, 0, &bad));
    g[0].start_address = 0; g[0].interval_ms = 500;
                               TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_GROUP_TIMING, ems_regmap_validate(g, 1, NULL, 0, &bad));
    g[0].interval_ms = 5000; g[0].stale_timeout_ms = 5000;
                               TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_GROUP_TIMING, ems_regmap_validate(g, 1, NULL, 0, &bad));
    TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_NULL, ems_regmap_validate(NULL, 1, NULL, 0, &bad));
    TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_NULL, ems_regmap_validate(g, 0, NULL, 0, &bad));
    TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_NULL, ems_regmap_validate(g, 1, NULL, 1, &bad));
}

static void test_validate_rejects_bad_mappings(void)
{
    ems_reg_mapping_t m = k_maps[6];   /* active power, INT32 at 6 in group 0 */
    TEST_ASSERT_EQUAL_INT(EMS_MAP_OK, validate_with(&m));

    m = k_maps[6]; m.group_index = 3;          TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_GROUP_REF, validate_with(&m));
    m = k_maps[6]; m.type = 99;                TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_TYPE, validate_with(&m));
    m = k_maps[0]; m.order = EMS_ORDER_CDAB;   TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_ORDER, validate_with(&m));
    m = k_maps[6]; m.address = 13;             TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_OUTSIDE_BLOCK, validate_with(&m)); /* CFG-008 */
    m = k_maps[11]; m.address = 13;            TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_OUTSIDE_BLOCK, validate_with(&m)); /* before block */
    m = k_maps[6]; m.scale = 0.0;              TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_SCALE, validate_with(&m));         /* CFG-009 */
    m = k_maps[6]; m.offset = (double)NAN;     TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_SCALE, validate_with(&m));
    m = k_maps[6]; m.scale = (double)INFINITY; TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_SCALE, validate_with(&m));
    m = k_maps[6]; m.min = 5.0; m.max = 5.0;   TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_RANGE, validate_with(&m));
    m = k_maps[6]; m.max = (double)NAN;        TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_RANGE, validate_with(&m));
    m = k_maps[0]; m.bit_mask = 0x00F0;        TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_MASK, validate_with(&m));
    m = k_maps[6]; m.flags = EMS_MAP_F_COUNTER;   TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_FLAGS, validate_with(&m)); /* INT32 counter */
    m = k_maps[0]; m.flags = EMS_MAP_F_ROLLOVER | EMS_MAP_F_COUNTER;
                                               TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_FLAGS, validate_with(&m));      /* UINT16 rollover */
    m = k_maps[11]; m.flags = EMS_MAP_F_ROLLOVER; TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_FLAGS, validate_with(&m)); /* rollover without counter */
    m = k_maps[11]; m.flags = EMS_MAP_F_ROLLOVER | EMS_MAP_F_COUNTER;
                                               TEST_ASSERT_EQUAL_INT(EMS_MAP_OK, validate_with(&m));

    /* Duplicate mapping_id (CFG-006), reported at the second occurrence. */
    ems_reg_mapping_t two[2] = { k_maps[0], k_maps[1] };
    two[1].mapping_id = two[0].mapping_id;
    size_t bad = 99;
    TEST_ASSERT_EQUAL_INT(EMS_MAP_ERR_DUPLICATE_ID, ems_regmap_validate(k_groups, N_GROUPS, two, 2, &bad));
    TEST_ASSERT_EQUAL_UINT32(1u, bad);
}

static void test_check_names(void)
{
    TEST_ASSERT_EQUAL_STRING("OK", ems_map_check_name(EMS_MAP_OK));
    TEST_ASSERT_EQUAL_STRING("ERR_OUTSIDE_BLOCK", ems_map_check_name(EMS_MAP_ERR_OUTSIDE_BLOCK));
    TEST_ASSERT_EQUAL_STRING("ERR_DUPLICATE_ID", ems_map_check_name(EMS_MAP_ERR_DUPLICATE_ID));
    TEST_ASSERT_EQUAL_STRING("ERR_UNKNOWN", ems_map_check_name((ems_map_check_t)99));
}

void run_register_map_tests(void)
{
    RUN_TEST(test_DEC001_uint16);
    RUN_TEST(test_DEC002_int16_positive_and_negative);
    RUN_TEST(test_DEC003_int32_abcd_negative);
    RUN_TEST(test_DEC004_int32_cdab);
    RUN_TEST(test_DEC005_uint32_abcd_and_cdab);
    RUN_TEST(test_DEC006_float32_all_four_orders);
    RUN_TEST(test_DEC007_float32_nan_and_inf_are_invalid);
    RUN_TEST(test_DEC008_bitfield_mask_and_shift);
    RUN_TEST(test_DEC009_short_response_is_rejected_for_the_whole_group);
    RUN_TEST(test_DEC010_last_registers_of_a_125_register_group);
    RUN_TEST(test_decode_rejects_bad_type_order_and_null);
    RUN_TEST(test_CONV_ac_meter_vectors_test_spec_4_1);
    RUN_TEST(test_CONV_decimal_scales_are_exact);
    RUN_TEST(test_CONV001_scale_and_offset);
    RUN_TEST(test_CONV002_range_limits_are_inclusive);
    RUN_TEST(test_CONV003_outside_range_is_out_of_range_with_value);
    RUN_TEST(test_CONV004_ratio_vectors_test_spec_4_3);
    RUN_TEST(test_CONV005_meter_reports_primary_means_no_ratio);
    RUN_TEST(test_CONV006_ratio_is_applied_exactly_once);
    RUN_TEST(test_CONV007_largest_energy_counter_is_exact);
    RUN_TEST(test_CONV008_power_sign_and_pf_sign);
    RUN_TEST(test_convert_non_finite_result_is_invalid);
    RUN_TEST(test_validate_accepts_the_ac_meter_map);
    RUN_TEST(test_validate_rejects_bad_groups);
    RUN_TEST(test_validate_rejects_bad_mappings);
    RUN_TEST(test_check_names);
}
