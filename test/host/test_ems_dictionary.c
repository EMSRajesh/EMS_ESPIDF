/*
 * Dictionary and record tests (DP-WP4 4B).
 * Covers UMD-001, UMD-003, UMD-004 and the text helpers.
 * UMD-002 (ID stability) is tested on the generator: tools/profile/tests.
 */
#include <stddef.h>
#include <string.h>

#include "unity.h"
#include "ems_measurement.h"

typedef struct {
    const char *name;
    uint16_t id;
    ems_unit_t unit;
    ems_value_type_t vt;
} expected_param_t;

/* Design Specification v1.1, Section 6.1 */
static const expected_param_t k_expected[] = {
    { "dev.status.code",         0x0001u, EMS_UNIT_NONE,  EMS_VT_ENUM    },
    { "ac.voltage.l1_n",         0x0101u, EMS_UNIT_V,     EMS_VT_FLOAT   },
    { "ac.voltage.l2_n",         0x0102u, EMS_UNIT_V,     EMS_VT_FLOAT   },
    { "ac.voltage.l3_n",         0x0103u, EMS_UNIT_V,     EMS_VT_FLOAT   },
    { "ac.current.l1",           0x0111u, EMS_UNIT_A,     EMS_VT_FLOAT   },
    { "ac.current.l2",           0x0112u, EMS_UNIT_A,     EMS_VT_FLOAT   },
    { "ac.current.l3",           0x0113u, EMS_UNIT_A,     EMS_VT_FLOAT   },
    { "ac.power.active.total",   0x0121u, EMS_UNIT_KW,    EMS_VT_FLOAT   },
    { "ac.power.reactive.total", 0x0122u, EMS_UNIT_KVAR,  EMS_VT_FLOAT   },
    { "ac.power.apparent.total", 0x0123u, EMS_UNIT_KVA,   EMS_VT_FLOAT   },
    { "ac.power.factor.total",   0x0124u, EMS_UNIT_RATIO, EMS_VT_FLOAT   },
    { "ac.frequency",            0x0130u, EMS_UNIT_HZ,    EMS_VT_FLOAT   },
    { "ac.energy.import.active", 0x0140u, EMS_UNIT_KWH,   EMS_VT_COUNTER },
    { "ac.energy.export.active", 0x0141u, EMS_UNIT_KWH,   EMS_VT_COUNTER },
};
#define N_EXPECTED (sizeof(k_expected) / sizeof(k_expected[0]))

static void test_UMD001_every_wp4_parameter_resolves_by_name_and_id(void)
{
    TEST_ASSERT_EQUAL_UINT32(N_EXPECTED, ems_param_table_len);
    TEST_ASSERT_EQUAL_UINT32(EMS_PARAM_COUNT, ems_param_table_len);
    for (size_t i = 0; i < N_EXPECTED; ++i) {
        const ems_param_info_t *by_name = ems_param_find_by_name(k_expected[i].name);
        TEST_ASSERT_NOT_NULL_MESSAGE(by_name, k_expected[i].name);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(k_expected[i].id, by_name->runtime_id, k_expected[i].name);
        const ems_param_info_t *by_id = ems_param_find(k_expected[i].id);
        TEST_ASSERT_TRUE_MESSAGE(by_id == by_name, k_expected[i].name);
    }
}

static void test_UMD001_generated_macros_match_design_ids(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x0001u, EMS_PARAM_DEV_STATUS_CODE);
    TEST_ASSERT_EQUAL_HEX16(0x0101u, EMS_PARAM_AC_VOLTAGE_L1_N);
    TEST_ASSERT_EQUAL_HEX16(0x0113u, EMS_PARAM_AC_CURRENT_L3);
    TEST_ASSERT_EQUAL_HEX16(0x0121u, EMS_PARAM_AC_POWER_ACTIVE_TOTAL);
    TEST_ASSERT_EQUAL_HEX16(0x0124u, EMS_PARAM_AC_POWER_FACTOR_TOTAL);
    TEST_ASSERT_EQUAL_HEX16(0x0130u, EMS_PARAM_AC_FREQUENCY);
    TEST_ASSERT_EQUAL_HEX16(0x0141u, EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE);
    TEST_ASSERT_EQUAL_STRING("1.1.0", EMS_UMD_VERSION);   /* DP-WP5: roles and capabilities added */
}

static void test_UMD001_table_sorted_unique_and_domain_consistent(void)
{
    for (size_t i = 0; i < ems_param_table_len; ++i) {
        const ems_param_info_t *p = &ems_param_table[i];
        if (i > 0u) {
            TEST_ASSERT_TRUE(ems_param_table[i - 1u].runtime_id < p->runtime_id);
        }
        /* High byte = domain: "dev." -> 0x00, "ac." -> 0x01 */
        if (strncmp(p->name, "ac.", 3) == 0) {
            TEST_ASSERT_EQUAL_HEX8(EMS_DOMAIN_AC, (uint8_t)(p->runtime_id >> 8));
        } else if (strncmp(p->name, "dev.", 4) == 0) {
            TEST_ASSERT_EQUAL_HEX8(EMS_DOMAIN_DEV, (uint8_t)(p->runtime_id >> 8));
        } else {
            TEST_FAIL_MESSAGE("unexpected domain in WP4 dictionary");
        }
        TEST_ASSERT_NOT_NULL(p->description);
        TEST_ASSERT_TRUE(strlen(p->description) > 0u);
    }
}

static void test_UMD004_units_and_value_types(void)
{
    for (size_t i = 0; i < N_EXPECTED; ++i) {
        const ems_param_info_t *p = ems_param_find(k_expected[i].id);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL_INT_MESSAGE(k_expected[i].unit, p->unit, k_expected[i].name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(k_expected[i].vt, p->value_type, k_expected[i].name);
    }
}

static void test_lookup_of_unknown_parameter_returns_null(void)
{
    TEST_ASSERT_NULL(ems_param_find(0x0000u));
    TEST_ASSERT_NULL(ems_param_find(0x0104u));   /* reserved, not mapped in WP4 */
    TEST_ASSERT_NULL(ems_param_find(0xFFFFu));
    TEST_ASSERT_NULL(ems_param_find_by_name("ac.voltage.l4_n"));
    TEST_ASSERT_NULL(ems_param_find_by_name(""));
    TEST_ASSERT_NULL(ems_param_find_by_name(NULL));
}

static void test_UMD003_record_layout_is_40_bytes(void)
{
    TEST_ASSERT_EQUAL_UINT32(40u, sizeof(ems_measurement_record_t));
    TEST_ASSERT_EQUAL_UINT32(0u,  offsetof(ems_measurement_record_t, param_id));
    TEST_ASSERT_EQUAL_UINT32(2u,  offsetof(ems_measurement_record_t, device_slot));
    TEST_ASSERT_EQUAL_UINT32(3u,  offsetof(ems_measurement_record_t, quality));
    TEST_ASSERT_EQUAL_UINT32(4u,  offsetof(ems_measurement_record_t, seq));
    TEST_ASSERT_EQUAL_UINT32(8u,  offsetof(ems_measurement_record_t, value));
    TEST_ASSERT_EQUAL_UINT32(16u, offsetof(ems_measurement_record_t, sample_mono_us));
    TEST_ASSERT_EQUAL_UINT32(24u, offsetof(ems_measurement_record_t, sample_utc_ms));
    TEST_ASSERT_EQUAL_UINT32(32u, offsetof(ems_measurement_record_t, utc_valid));
}

static void test_only_good_is_usable(void)
{
    for (int q = 0; q < (int)EMS_Q_COUNT; ++q) {
        TEST_ASSERT_EQUAL(q == (int)EMS_Q_GOOD, ems_quality_is_usable((ems_quality_t)q));
    }
}

static void test_text_helpers_cover_every_value(void)
{
    TEST_ASSERT_EQUAL_STRING("INITIALIZING", ems_quality_name(EMS_Q_INITIALIZING));
    TEST_ASSERT_EQUAL_STRING("GOOD", ems_quality_name(EMS_Q_GOOD));
    TEST_ASSERT_EQUAL_STRING("STALE", ems_quality_name(EMS_Q_STALE));
    TEST_ASSERT_EQUAL_STRING("COMM_LOST", ems_quality_name(EMS_Q_COMM_LOST));
    TEST_ASSERT_EQUAL_STRING("INVALID", ems_quality_name(EMS_Q_INVALID));
    TEST_ASSERT_EQUAL_STRING("OUT_OF_RANGE", ems_quality_name(EMS_Q_OUT_OF_RANGE));
    TEST_ASSERT_EQUAL_STRING("NOT_SUPPORTED", ems_quality_name(EMS_Q_NOT_SUPPORTED));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", ems_quality_name(EMS_Q_COUNT));

    TEST_ASSERT_EQUAL_STRING("kvar", ems_unit_symbol(EMS_UNIT_KVAR));
    TEST_ASSERT_EQUAL_STRING("1", ems_unit_symbol(EMS_UNIT_RATIO));
    TEST_ASSERT_EQUAL_STRING("-", ems_unit_symbol(EMS_UNIT_NONE));
    TEST_ASSERT_EQUAL_STRING("?", ems_unit_symbol(EMS_UNIT_COUNT));
    for (int u = 0; u < (int)EMS_UNIT_COUNT; ++u) {
        TEST_ASSERT_TRUE(strcmp("?", ems_unit_symbol((ems_unit_t)u)) != 0);
    }
    TEST_ASSERT_EQUAL_STRING("EMS_OK", ems_status_name(EMS_OK));
    TEST_ASSERT_EQUAL_STRING("EMS_ERR_NO_SPACE", ems_status_name(EMS_ERR_NO_SPACE));
}

void run_dictionary_tests(void)
{
    RUN_TEST(test_UMD001_every_wp4_parameter_resolves_by_name_and_id);
    RUN_TEST(test_UMD001_generated_macros_match_design_ids);
    RUN_TEST(test_UMD001_table_sorted_unique_and_domain_consistent);
    RUN_TEST(test_UMD004_units_and_value_types);
    RUN_TEST(test_lookup_of_unknown_parameter_returns_null);
    RUN_TEST(test_UMD003_record_layout_is_40_bytes);
    RUN_TEST(test_only_good_is_usable);
    RUN_TEST(test_text_helpers_cover_every_value);
}
