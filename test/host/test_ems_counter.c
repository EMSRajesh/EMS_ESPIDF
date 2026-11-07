/*
 * Energy counter tests (DP-WP4 4C, Design 8.5): ENG-001..ENG-007.
 * ENG-008 (simulator COUNTER_RESET) is a HIL test.
 */
#include "unity.h"
#include "ems_counter.h"

#define S(x) ((int64_t)(x) * 1000000)     /* seconds -> us */
#define TOL 1e-9

static ems_counter_t c;

static void start(bool rollover, uint32_t raw, double value)
{
    ems_counter_init(&c, rollover, 0.01);
    double d = -1.0;
    TEST_ASSERT_EQUAL_INT(EMS_CNT_FIRST, ems_counter_update(&c, raw, value, S(100), 0.0, &d));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, d);
}

static void test_ENG001_increase_gives_delta(void)
{
    start(false, 10000u, 100.00);
    double d = 0.0;
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DELTA, ems_counter_update(&c, 10025u, 100.25, S(110), 0.0, &d));
    TEST_ASSERT_DOUBLE_WITHIN(TOL, 0.25, d);
    TEST_ASSERT_DOUBLE_WITHIN(TOL, 0.25, c.last_delta);
}

static void test_ENG002_unchanged_counter_gives_zero_delta(void)
{
    start(false, 10025u, 100.25);
    double d = -1.0;
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DELTA, ems_counter_update(&c, 10025u, 100.25, S(110), 0.0, &d));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, d);
}

static void test_ENG003_reset_to_zero_is_a_discontinuity(void)
{
    start(false, 10025u, 100.25);
    double d = -1.0;
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DISCONTINUITY, ems_counter_update(&c, 0u, 0.0, S(110), 0.0, &d));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, d);                /* never a negative consumption */
    /* New baseline: the next increase is measured from zero. */
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DELTA, ems_counter_update(&c, 5u, 0.05, S(120), 0.0, &d));
    TEST_ASSERT_DOUBLE_WITHIN(TOL, 0.05, d);
}

static void test_ENG004_rollover_when_enabled_and_plausible(void)
{
    start(true, 0xFFFFFF00u, 42949670.40);
    double d = 0.0;
    /* 10 s at up to 1000 kW: limit 2 x 1000 x 10/3600 = 5.56 kWh >= 2.72 */
    TEST_ASSERT_EQUAL_INT(EMS_CNT_ROLLOVER,
        ems_counter_update(&c, 0x00000010u, 0.16, S(110), 1000.0, &d));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 2.72, d);        /* 0x110 counts x 0.01 kWh */
    /* No plausibility limit given: rollover accepted. */
    start(true, 0xFFFFFFFFu, 42949672.95);
    TEST_ASSERT_EQUAL_INT(EMS_CNT_ROLLOVER, ems_counter_update(&c, 0u, 0.0, S(101), 0.0, &d));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.01, d);
}

static void test_ENG005_implausible_rollover_is_a_discontinuity(void)
{
    start(true, 0xFFFFFF00u, 42949670.40);
    double d = -1.0;
    /* 1 s at up to 1000 kW: limit 0.56 kWh < 2.72 */
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DISCONTINUITY,
        ems_counter_update(&c, 0x00000010u, 0.16, S(101), 1000.0, &d));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, d);
    /* Zero or negative elapsed time can never justify a rollover. */
    start(true, 0xFFFFFF00u, 42949670.40);
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DISCONTINUITY,
        ems_counter_update(&c, 0x00000010u, 0.16, S(100), 1000.0, &d));
    /* Rollover disabled: any decrease is a discontinuity. */
    start(false, 0xFFFFFF00u, 42949670.40);
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DISCONTINUITY,
        ems_counter_update(&c, 0x00000010u, 0.16, S(110), 0.0, &d));
}

static void test_ENG006_decrease_by_one_count_is_flagged_and_kept(void)
{
    start(false, 10025u, 100.25);
    double d = -1.0;
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DISCONTINUITY, ems_counter_update(&c, 10024u, 100.24, S(110), 0.0, &d));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, d);
    TEST_ASSERT_EQUAL_UINT32(10024u, c.last_raw);    /* raw value preserved as new baseline */
    TEST_ASSERT_DOUBLE_WITHIN(TOL, 100.24, c.last_value);
}

static void test_ENG007_failed_read_leaves_baseline_and_time_unchanged(void)
{
    /* The caller does not call update after a failed read; the tracker must
     * therefore keep the last GOOD baseline and timestamp. */
    start(false, 10000u, 100.00);
    const ems_counter_t before = c;
    /* ... read fails: no call ... next GOOD read 30 s later: */
    double d = 0.0;
    TEST_ASSERT_EQUAL_UINT32(before.last_raw, c.last_raw);
    TEST_ASSERT_EQUAL_INT64(S(100), c.last_mono_us);
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DELTA, ems_counter_update(&c, 10050u, 100.50, S(130), 0.0, &d));
    TEST_ASSERT_DOUBLE_WITHIN(TOL, 0.50, d);          /* delta spans the gap, nothing lost */
    TEST_ASSERT_EQUAL_INT64(S(130), c.last_mono_us);
}

static void test_counter_null_safety(void)
{
    double d = -1.0;
    ems_counter_init(NULL, true, 1.0);
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DISCONTINUITY, ems_counter_update(NULL, 1u, 1.0, 0, 0.0, &d));
    TEST_ASSERT_EQUAL_DOUBLE(0.0, d);
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DISCONTINUITY, ems_counter_update(NULL, 1u, 1.0, 0, 0.0, NULL));
    start(false, 1u, 0.01);
    TEST_ASSERT_EQUAL_INT(EMS_CNT_DELTA, ems_counter_update(&c, 2u, 0.02, S(101), 0.0, NULL));
}

void run_counter_tests(void)
{
    RUN_TEST(test_ENG001_increase_gives_delta);
    RUN_TEST(test_ENG002_unchanged_counter_gives_zero_delta);
    RUN_TEST(test_ENG003_reset_to_zero_is_a_discontinuity);
    RUN_TEST(test_ENG004_rollover_when_enabled_and_plausible);
    RUN_TEST(test_ENG005_implausible_rollover_is_a_discontinuity);
    RUN_TEST(test_ENG006_decrease_by_one_count_is_flagged_and_kept);
    RUN_TEST(test_ENG007_failed_read_leaves_baseline_and_time_unchanged);
    RUN_TEST(test_counter_null_safety);
}
