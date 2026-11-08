/*
 * Acquisition core tests (DP-WP4 D6/D7): routing (RTE), result -> quality (Q),
 * dictionary/record (UMD), ratios through the full path (CONV-004..006),
 * counters through the full path (ENG), device state (DEV) and the
 * acquisition counters (DIAG-001/002). Uses the generated lab_bench tables
 * (main/generated) and modified copies of them.
 */
#include <string.h>
#include "unity.h"
#include "ems_acq_core.h"
#include "ems_config_gen.h"
#include "ems_param_ids.h"

static uint64_t dbits(double d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}
#define ASSERT_SAME_DOUBLE(e, a) TEST_ASSERT_EQUAL_HEX64(dbits(e), dbits(a))
#define S(x) ((int64_t)((x) * 1000000.0))

/* ---------------------------------------------------------- fixtures --- */
static ems_acq_core_t   A;
static ems_sched_plan_t P;
static ems_config_build_t   C;            /* modifiable copy of ems_config_build */
static ems_cfg_device_t     C_dev[4];
static ems_cfg_profile_t    C_prof[2];
static ems_cfg_map_t        C_map[2];
static ems_reg_mapping_t    C_mappings[32];

/* Test Spec 4.1 register image, addresses 0..18 */
static uint16_t R[19];
static void image_4_1(void)
{
    static const uint16_t v[19] = { 0x08FC, 0x0906, 0x08F7, 0x03E8, 0x03F2, 0x03DE, 0x0000, 0x30D4,
                                    0x0000, 0x0BB8, 0x0000, 0x3237, 0x03CC, 0x1388, 0x0000, 0x2729,
                                    0x0000, 0x0000, 0x0000 };
    memcpy(R, v, sizeof(v));
}

static void copy_cfg(void)
{
    C = ems_config_build;
    memcpy(C_dev, ems_config_build.devices, sizeof(C_dev[0]) * ems_config_build.device_count);
    memcpy(C_prof, ems_config_build.profiles, sizeof(C_prof[0]) * ems_config_build.profile_count);
    memcpy(C_map, ems_config_build.maps, sizeof(C_map[0]) * ems_config_build.map_count);
    memcpy(C_mappings, ems_config_build.maps[0].mappings,
           sizeof(C_mappings[0]) * ems_config_build.maps[0].mapping_count);
    C.devices = C_dev;
    C.profiles = C_prof;
    C.maps = C_map;
    C_map[0].mappings = C_mappings;
}

static void start(void)
{
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_OK, ems_sched_plan_build(&C, &P));
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_INIT_OK, ems_acq_core_init(&A, &C, &P, NULL));
    image_4_1();
}

static void start_lab(void)
{
    copy_cfg();
    start();
}

/* Build a result for poll entry e from the current image R. */
static ems_acq_result_t ok_result(uint8_t e, uint32_t seq)
{
    const ems_sched_entry_t *en = &P.entries[e];
    ems_acq_result_t r = {
        .registers = &R[en->start_address], .request_id = ems_sched_make_request_id(e, seq),
        .register_count = en->count, .slave_addr = en->slave_id, .function_code = en->function_code,
        .status = EMS_ACQ_RES_OK, .exception_code = 0u };
    return r;
}

static ems_acq_result_t status_result(uint8_t e, uint8_t status, uint8_t exc)
{
    ems_acq_result_t r = ok_result(e, 7u);
    r.status = status;
    r.exception_code = exc;
    r.register_count = 0u;
    r.registers = NULL;
    return r;
}

static ems_measurement_record_t rec(uint8_t slot, uint16_t id)
{
    ems_measurement_record_t r;
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_get(slot, id, &r));
    return r;
}

static void assert_q(uint8_t slot, uint16_t id, ems_quality_t q)
{
    TEST_ASSERT_EQUAL_STRING(ems_quality_name(q), ems_quality_name((ems_quality_t)rec(slot, id).quality));
}

static void feed_all(int64_t t)
{
    ems_acq_outcome_t o;
    for (uint8_t e = 0; e < P.entry_count; e++) {
        ems_acq_result_t r = ok_result(e, 1u);
        ems_acq_core_process(&A, &r, t, &o);
        TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_OK, o.kind);
    }
}

/* -------------------------------------------------------------- init --- */
static void test_init_layout_and_Q001_initializing(void)
{
    start_lab();
    TEST_ASSERT_EQUAL_UINT8(14u, A.devices[0].param_count);
    TEST_ASSERT_EQUAL_UINT32(11000u, A.params[0][0].stale_timeout_ms);    /* electrical */
    TEST_ASSERT_EQUAL_UINT32(21000u, A.params[0][11].stale_timeout_ms);   /* energy     */
    TEST_ASSERT_EQUAL_UINT32(61000u, A.params[0][13].stale_timeout_ms);   /* status     */
    ems_measurement_record_t r = rec(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_INITIALIZING, r.quality);
    TEST_ASSERT_EQUAL_INT64(0, r.sample_mono_us);
    /* not in this device's map -> NOT_SUPPORTED (UMD-005 style) */
    TEST_ASSERT_TRUE(A.counter_of[0][11] != EMS_ACQ_NO_COUNTER);
    TEST_ASSERT_TRUE(A.counter_of[0][12] != EMS_ACQ_NO_COUNTER);
    TEST_ASSERT_TRUE(A.counter_of[0][0] == EMS_ACQ_NO_COUNTER);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_INIT_NULL, ems_acq_core_init(NULL, &C, &P, NULL));
    copy_cfg();
    C_dev[0].profile_index = 5u;
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_INIT_BAD_CONFIG, ems_acq_core_init(&A, &C, &P, NULL));
    TEST_ASSERT_EQUAL_STRING("BAD_CONFIG", ems_acq_init_status_name(EMS_ACQ_INIT_BAD_CONFIG));
}

/* ---------------------------------------------------- UMD / Q-002 --- */
static void test_UMD006_Q002_vectors_4_1_published_good(void)
{
    start_lab();
    feed_all(S(100));
    static const struct { uint16_t id; double v; } exp[] = {
        { EMS_PARAM_AC_VOLTAGE_L1_N, 230.0 }, { EMS_PARAM_AC_VOLTAGE_L2_N, 231.0 },
        { EMS_PARAM_AC_VOLTAGE_L3_N, 229.5 }, { EMS_PARAM_AC_CURRENT_L1, 10.0 },
        { EMS_PARAM_AC_CURRENT_L2, 10.1 }, { EMS_PARAM_AC_CURRENT_L3, 9.9 },
        { EMS_PARAM_AC_POWER_ACTIVE_TOTAL, 12.5 }, { EMS_PARAM_AC_POWER_REACTIVE_TOTAL, 3.0 },
        { EMS_PARAM_AC_POWER_APPARENT_TOTAL, 12.855 }, { EMS_PARAM_AC_POWER_FACTOR_TOTAL, 0.972 },
        { EMS_PARAM_AC_FREQUENCY, 50.0 }, { EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, 100.25 },
        { EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE, 0.0 }, { EMS_PARAM_DEV_STATUS_CODE, 0.0 },
    };
    for (size_t i = 0; i < sizeof(exp) / sizeof(exp[0]); i++) {
        ems_measurement_record_t r = rec(0, exp[i].id);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(EMS_Q_GOOD, r.quality, ems_param_find(exp[i].id)->name);
        ASSERT_SAME_DOUBLE(exp[i].v, r.value);
        TEST_ASSERT_EQUAL_INT64(S(100), r.sample_mono_us);      /* UMD-008 */
        TEST_ASSERT_EQUAL_UINT8(0u, r.utc_valid);
    }
    TEST_ASSERT_EQUAL_UINT32(3u, A.dev[0].ok);
    TEST_ASSERT_EQUAL_INT64(S(100), A.dev[0].last_ok_us);
}

static void test_UMD007_export_and_CONV008_sign(void)
{
    start_lab();
    feed_all(S(1));
    R[6] = 0xFFFF; R[7] = 0xCF2C; R[12] = 0xFC34;   /* -12.5 kW, PF -0.972 */
    R[16] = 0x0000; R[17] = 0x0019;                  /* export 0.25 kWh     */
    feed_all(S(11));
    ASSERT_SAME_DOUBLE(-12.5, rec(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL).value);
    ASSERT_SAME_DOUBLE(-0.972, rec(0, EMS_PARAM_AC_POWER_FACTOR_TOTAL).value);
    assert_q(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL, EMS_Q_GOOD);
    ASSERT_SAME_DOUBLE(0.25, rec(0, EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE).value);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.25, ems_acq_core_counter_delta(&A, 0, EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE));
}

static void test_UMD009_seq_increments_on_every_write(void)
{
    start_lab();
    ems_acq_result_t r = ok_result(0, 1u);
    ems_acq_core_process(&A, &r, S(1), NULL);
    const uint32_t s1 = rec(0, EMS_PARAM_AC_VOLTAGE_L1_N).seq;
    ems_acq_core_process(&A, &r, S(6), NULL);                   /* same value */
    TEST_ASSERT_EQUAL_UINT32(s1 + 1u, rec(0, EMS_PARAM_AC_VOLTAGE_L1_N).seq);
}

/* --------------------------------------------------------------- RTE --- */
static void test_RTE002_each_group_updates_only_its_parameters(void)
{
    start_lab();
    ems_acq_outcome_t o;
    ems_acq_result_t r = ok_result(1, 1u);                        /* energy */
    ems_acq_core_process(&A, &r, S(1), &o);
    TEST_ASSERT_EQUAL_UINT8(2u, o.good);
    TEST_ASSERT_EQUAL_UINT8(1u, o.group_index);
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_GOOD);
    assert_q(0, EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE, EMS_Q_GOOD);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_INITIALIZING);
    assert_q(0, EMS_PARAM_DEV_STATUS_CODE, EMS_Q_INITIALIZING);
    r = ok_result(0, 2u);
    ems_acq_core_process(&A, &r, S(2), &o);
    TEST_ASSERT_EQUAL_UINT8(11u, o.good);
    r = ok_result(2, 3u);
    ems_acq_core_process(&A, &r, S(3), &o);
    TEST_ASSERT_EQUAL_UINT8(1u, o.good);
}

static void test_RTE003_wrong_register_count(void)
{
    start_lab();
    feed_all(S(1));
    ems_acq_outcome_t o;
    ems_acq_result_t r = ok_result(0, 2u);
    r.register_count = 13u;
    const uint32_t seq_before = rec(0, EMS_PARAM_AC_VOLTAGE_L1_N).seq;
    ems_acq_core_process(&A, &r, S(2), &o);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_MISMATCH, o.kind);
    TEST_ASSERT_EQUAL_UINT32(1u, A.stats.routing_errors);
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].mismatches);
    TEST_ASSERT_EQUAL_UINT8(11u, o.invalid);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_INVALID);
    ASSERT_SAME_DOUBLE(230.0, rec(0, EMS_PARAM_AC_VOLTAGE_L1_N).value);   /* nothing decoded */
    TEST_ASSERT_TRUE(rec(0, EMS_PARAM_AC_VOLTAGE_L1_N).seq > seq_before);
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_GOOD);           /* other group  */
}

static void test_RTE004_unknown_entry_and_wrong_slave(void)
{
    start_lab();
    feed_all(S(1));
    ems_acq_outcome_t o;
    ems_acq_result_t r = ok_result(0, 2u);
    r.request_id = ems_sched_make_request_id(9u, 2u);              /* no entry 9 */
    ems_acq_core_process(&A, &r, S(2), &o);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_ROUTING_ERROR, o.kind);
    TEST_ASSERT_EQUAL_UINT8(EMS_SCHED_NO_ENTRY, o.entry);
    r = ok_result(0, 3u);
    r.slave_addr = 2u;
    ems_acq_core_process(&A, &r, S(2), &o);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_ROUTING_ERROR, o.kind);
    r = ok_result(0, 4u);
    r.function_code = 4u;
    ems_acq_core_process(&A, &r, S(2), &o);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_ROUTING_ERROR, o.kind);
    TEST_ASSERT_EQUAL_UINT32(3u, A.stats.routing_errors);
    TEST_ASSERT_EQUAL_INT64(S(1), rec(0, EMS_PARAM_AC_VOLTAGE_L1_N).sample_mono_us);  /* unchanged */
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_GOOD);
}

static void test_RTE005_two_devices_no_cross_talk(void)
{
    copy_cfg();
    C_dev[1] = C_dev[0];
    C_dev[1].slot = 1u;
    C_dev[1].slave_id = 2u;
    C_dev[1].instance_id = "FEEDER_MCC_01";
    C.device_count = 2u;
    start();
    TEST_ASSERT_EQUAL_UINT8(6u, P.entry_count);
    ems_acq_result_t r = ok_result(3, 1u);                         /* slave 2, electrical */
    R[0] = 2400u;
    ems_acq_core_process(&A, &r, S(1), NULL);
    ASSERT_SAME_DOUBLE(240.0, rec(1, EMS_PARAM_AC_VOLTAGE_L1_N).value);
    assert_q(1, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_GOOD);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_INITIALIZING);
    r = ok_result(3, 2u);
    r.slave_addr = 1u;                                            /* slave 1 answering for slot 1 */
    ems_acq_core_process(&A, &r, S(2), NULL);
    TEST_ASSERT_EQUAL_UINT32(1u, A.stats.routing_errors);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_INITIALIZING);
}

/* ----------------------------------------------------------------- Q --- */
static void test_Q003_timeout_after_good_gives_stale_keeps_value(void)
{
    start_lab();
    feed_all(S(1));
    ems_acq_outcome_t o;
    ems_acq_result_t r = status_result(0, EMS_ACQ_RES_TIMEOUT, 0);
    ems_acq_core_process(&A, &r, S(6), &o);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_FAILED, o.kind);
    TEST_ASSERT_EQUAL_UINT8(11u, o.stale);
    TEST_ASSERT_EQUAL_UINT8(1u, o.consecutive_failures);
    ems_measurement_record_t v = rec(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_STALE, v.quality);
    ASSERT_SAME_DOUBLE(12.5, v.value);                            /* last GOOD value kept */
    TEST_ASSERT_EQUAL_INT64(S(1), v.sample_mono_us);              /* and its timestamp    */
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_GOOD);   /* other group unchanged */
    /* other failure kinds count the same way */
    r = status_result(1, EMS_ACQ_RES_CRC_ERROR, 0);
    ems_acq_core_process(&A, &r, S(7), &o);
    TEST_ASSERT_EQUAL_UINT8(2u, o.consecutive_failures);
    r = status_result(2, EMS_ACQ_RES_PROTOCOL_ERROR, 0);
    ems_acq_core_process(&A, &r, S(8), &o);
    r = status_result(0, EMS_ACQ_RES_TRANSPORT_ERROR, 0);
    ems_acq_core_process(&A, &r, S(9), &o);
    TEST_ASSERT_EQUAL_UINT8(4u, o.consecutive_failures);
    TEST_ASSERT_EQUAL_UINT8(0u, o.stale);                          /* already STALE */
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].timeouts);
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].crc_errors);
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].protocol_errors);
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].transport_errors);
}

static void test_Q003_failure_before_first_sample_stays_initializing(void)
{
    start_lab();
    ems_acq_result_t r = status_result(0, EMS_ACQ_RES_TIMEOUT, 0);
    ems_acq_core_process(&A, &r, S(1), NULL);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_INITIALIZING);
}

static void test_Q004_freshness_turns_good_stale(void)
{
    start_lab();
    feed_all(S(10));
    TEST_ASSERT_EQUAL_UINT32(0u, ems_acq_core_tick(&A, S(10) + S(11.0)));       /* exactly 11 s: GOOD */
    TEST_ASSERT_EQUAL_UINT32(11u, ems_acq_core_tick(&A, S(10) + S(11.0) + 1));  /* electrical */
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_STALE);
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_GOOD);
    TEST_ASSERT_EQUAL_UINT32(2u, ems_acq_core_tick(&A, S(10) + S(21.0) + 1));   /* energy */
    TEST_ASSERT_EQUAL_UINT32(1u, ems_acq_core_tick(&A, S(10) + S(61.0) + 1));   /* status */
    TEST_ASSERT_EQUAL_UINT32(0u, ems_acq_core_tick(&A, S(1000)));
}

static void test_Q005_exception_gives_invalid_and_resets_failures(void)
{
    start_lab();
    feed_all(S(1));
    ems_acq_result_t t = status_result(1, EMS_ACQ_RES_TIMEOUT, 0);
    ems_acq_core_process(&A, &t, S(2), NULL);
    ems_acq_outcome_t o;
    ems_acq_result_t r = status_result(1, EMS_ACQ_RES_EXCEPTION, 2u);
    ems_acq_core_process(&A, &r, S(3), &o);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_EXCEPTION, o.kind);
    TEST_ASSERT_EQUAL_UINT8(2u, o.exception_code);
    TEST_ASSERT_EQUAL_UINT8(2u, o.invalid);
    TEST_ASSERT_EQUAL_UINT8(0u, o.consecutive_failures);
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_INVALID);
    assert_q(0, EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE, EMS_Q_INVALID);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_GOOD);
    TEST_ASSERT_EQUAL_UINT8(2u, A.dev[0].last_exception);
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].exceptions);
}

static void test_Q006_out_of_range(void)
{
    start_lab();
    R[13] = 3000u;                                                 /* 30.00 Hz */
    ems_acq_outcome_t o;
    ems_acq_result_t r = ok_result(0, 1u);
    ems_acq_core_process(&A, &r, S(1), &o);
    TEST_ASSERT_EQUAL_UINT8(10u, o.good);
    TEST_ASSERT_EQUAL_UINT8(1u, o.out_of_range);
    ems_measurement_record_t f = rec(0, EMS_PARAM_AC_FREQUENCY);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_OUT_OF_RANGE, f.quality);
    ASSERT_SAME_DOUBLE(30.0, f.value);                             /* kept for diagnostics */
    TEST_ASSERT_EQUAL_INT64(0, f.sample_mono_us);                  /* never GOOD */
}

static void test_Q007_recovery_after_stale(void)
{
    start_lab();
    feed_all(S(1));
    ems_acq_result_t t = status_result(0, EMS_ACQ_RES_TIMEOUT, 0);
    ems_acq_core_process(&A, &t, S(6), NULL);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_STALE);
    ems_acq_result_t r = ok_result(0, 9u);
    ems_acq_outcome_t o;
    ems_acq_core_process(&A, &r, S(11), &o);
    TEST_ASSERT_EQUAL_UINT8(0u, o.consecutive_failures);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_GOOD);
    TEST_ASSERT_EQUAL_INT64(S(11), rec(0, EMS_PARAM_AC_VOLTAGE_L1_N).sample_mono_us);
}

static void test_Q008_valid_zero_is_good(void)
{
    start_lab();
    R[6] = 0u; R[7] = 0u;
    feed_all(S(1));
    ems_measurement_record_t p = rec(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_GOOD, p.quality);
    ASSERT_SAME_DOUBLE(0.0, p.value);
}

static void test_Q010_cancelled_changes_nothing(void)
{
    start_lab();
    feed_all(S(1));
    const ems_measurement_record_t before = rec(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    ems_acq_outcome_t o;
    ems_acq_result_t r = status_result(0, EMS_ACQ_RES_CANCELLED, 0);
    ems_acq_core_process(&A, &r, S(2), &o);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_CANCELLED, o.kind);
    const ems_measurement_record_t after = rec(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
    TEST_ASSERT_EQUAL_UINT8(0u, o.consecutive_failures);
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].cancelled);
}

static void test_invalid_mapping_value_marks_only_that_parameter(void)
{
    copy_cfg();
    C_mappings[0].type = EMS_REG_FLOAT32;                          /* l1 voltage as FLOAT32 at 0..1 */
    C_mappings[0].order = EMS_ORDER_ABCD;
    C_mappings[0].scale = 1.0;
    start();
    R[0] = 0x7FC0u; R[1] = 0x0000u;                                /* NaN */
    ems_acq_outcome_t o;
    ems_acq_result_t r = ok_result(0, 1u);
    ems_acq_core_process(&A, &r, S(1), &o);
    TEST_ASSERT_EQUAL_UINT8(1u, o.invalid);
    TEST_ASSERT_EQUAL_UINT8(10u, o.good);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_INVALID);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L3_N, EMS_Q_GOOD);
}

/* -------------------------------------------------------------- CONV --- */
static void test_CONV004_006_ct200_vectors_4_3_full_path(void)
{
    copy_cfg();
    C_dev[0].ct_ratio = 200.0;
    start();
    R[3] = 250u; R[4] = 600u; R[6] = 0u; R[7] = 1000u;
    feed_all(S(1));
    ASSERT_SAME_DOUBLE(500.0, rec(0, EMS_PARAM_AC_CURRENT_L1).value);
    assert_q(0, EMS_PARAM_AC_CURRENT_L1, EMS_Q_GOOD);
    ASSERT_SAME_DOUBLE(1200.0, rec(0, EMS_PARAM_AC_CURRENT_L2).value);
    assert_q(0, EMS_PARAM_AC_CURRENT_L2, EMS_Q_OUT_OF_RANGE);
    ASSERT_SAME_DOUBLE(230.0, rec(0, EMS_PARAM_AC_VOLTAGE_L1_N).value);
    ASSERT_SAME_DOUBLE(200.0, rec(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL).value);
    ASSERT_SAME_DOUBLE(20050.0, rec(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE).value);  /* not x CT^2 */
    ASSERT_SAME_DOUBLE(50.0, rec(0, EMS_PARAM_AC_FREQUENCY).value);
}

static void test_CONV005_meter_reports_primary_no_ratio(void)
{
    copy_cfg();
    C_prof[0].ratio_handling = EMS_RATIO_METER_REPORTS_PRIMARY;
    C_dev[0].ct_ratio = 200.0;        /* V-12 forbids this; the core must still not apply it */
    start();
    feed_all(S(1));
    ASSERT_SAME_DOUBLE(10.0, rec(0, EMS_PARAM_AC_CURRENT_L1).value);
    ASSERT_SAME_DOUBLE(100.25, rec(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE).value);
}

/* --------------------------------------------------------------- ENG --- */
static void test_ENG_counters_through_full_path(void)
{
    start_lab();
    ems_acq_outcome_t o;
    ems_acq_result_t r = ok_result(1, 1u);
    ems_acq_core_process(&A, &r, S(0), &o);                         /* FIRST: 100.25 */
    ASSERT_SAME_DOUBLE(0.0, ems_acq_core_counter_delta(&A, 0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE));
    R[15] = 10050u;                                                /* ENG-001: +0.25 */
    ems_acq_core_process(&A, &r, S(10), &o);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.25, ems_acq_core_counter_delta(&A, 0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE));
    TEST_ASSERT_EQUAL_UINT8(0u, o.counter_discontinuities);
    ems_acq_core_process(&A, &r, S(20), &o);                        /* ENG-002: unchanged */
    ASSERT_SAME_DOUBLE(0.0, ems_acq_core_counter_delta(&A, 0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE));
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_GOOD);
    R[15] = 0u;                                                    /* ENG-003: reset */
    ems_acq_core_process(&A, &r, S(30), &o);
    TEST_ASSERT_EQUAL_UINT8(1u, o.counter_discontinuities);
    TEST_ASSERT_EQUAL_UINT16(EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, o.discontinuity_param);
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_GOOD);    /* raw counter is the truth */
    ASSERT_SAME_DOUBLE(0.0, rec(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE).value);
    ASSERT_SAME_DOUBLE(0.0, ems_acq_core_counter_delta(&A, 0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE));
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].counter_discontinuities);
    ems_acq_result_t t = status_result(1, EMS_ACQ_RES_TIMEOUT, 0);  /* ENG-007: failed read */
    ems_acq_core_process(&A, &t, S(40), &o);
    R[15] = 5u;
    ems_acq_core_process(&A, &r, S(50), &o);                        /* delta from baseline 0 */
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.05, ems_acq_core_counter_delta(&A, 0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE));
    TEST_ASSERT_EQUAL_UINT8(0u, o.counter_discontinuities);
    ASSERT_SAME_DOUBLE(0.0, ems_acq_core_counter_delta(&A, 0, EMS_PARAM_AC_VOLTAGE_L1_N));
}

static void test_ENG_out_of_range_counter_does_not_move_baseline(void)
{
    /* CT 2: 0xFFFFFFFF x 0.01 x 2 = 8.6e7 kWh > max 4.3e7 -> OUT_OF_RANGE */
    copy_cfg();
    C_dev[0].ct_ratio = 2.0;
    start();
    ems_acq_result_t r = ok_result(1, 1u);
    R[15] = 10025u;
    ems_acq_core_process(&A, &r, S(0), NULL);                       /* baseline 200.5 */
    R[14] = 0xFFFFu; R[15] = 0xFFFFu;
    ems_acq_outcome_t o;
    ems_acq_core_process(&A, &r, S(10), &o);
    TEST_ASSERT_EQUAL_UINT8(1u, o.out_of_range);
    TEST_ASSERT_EQUAL_UINT8(0u, o.counter_discontinuities);
    R[14] = 0u; R[15] = 10050u;
    ems_acq_core_process(&A, &r, S(20), &o);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.5, ems_acq_core_counter_delta(&A, 0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE));
}

static void test_disabled_device_not_supported(void)
{
    copy_cfg();
    C_dev[1] = C_dev[0];
    C_dev[1].slot = 1u;
    C_dev[1].slave_id = 2u;
    C_dev[1].enabled = false;
    C.device_count = 2u;
    start();
    TEST_ASSERT_EQUAL_UINT8(3u, P.entry_count);
    assert_q(1, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_NOT_SUPPORTED);
    TEST_ASSERT_EQUAL_STRING("OK", ems_acq_outcome_name(EMS_ACQ_OUT_OK));
}

/* --------------------------------------------------------- DEV (D7) --- */
static ems_acq_outcome_t fail_entry(uint8_t e, uint8_t status, int64_t t)
{
    ems_acq_outcome_t o;
    ems_acq_result_t r = status_result(e, status, 0);
    ems_acq_core_process(&A, &r, t, &o);
    return o;
}

static void assert_all(uint8_t slot, ems_quality_t q)
{
    static const uint16_t ids[] = {
        EMS_PARAM_AC_VOLTAGE_L1_N, EMS_PARAM_AC_CURRENT_L3, EMS_PARAM_AC_POWER_ACTIVE_TOTAL,
        EMS_PARAM_AC_FREQUENCY, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_PARAM_AC_ENERGY_EXPORT_ACTIVE,
        EMS_PARAM_DEV_STATUS_CODE };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        assert_q(slot, ids[i], q);
    }
}

static void test_DEV001_two_failures_take_device_offline(void)
{
    start_lab();
    feed_all(S(1));
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_ONLINE, ems_acq_core_device_mode(&A, 0));
    ems_acq_outcome_t o = fail_entry(0, EMS_ACQ_RES_TIMEOUT, S(6));
    TEST_ASSERT_FALSE(o.device_offline);                         /* threshold 2 */
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_STALE);
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_GOOD);
    o = fail_entry(1, EMS_ACQ_RES_TIMEOUT, S(9));
    TEST_ASSERT_TRUE(o.device_offline);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_OFFLINE, ems_acq_core_device_mode(&A, 0));
    assert_all(0, EMS_Q_COMM_LOST);                              /* every parameter */
    ASSERT_SAME_DOUBLE(12.5, rec(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL).value);   /* value kept */
    TEST_ASSERT_EQUAL_INT64(S(1), rec(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL).sample_mono_us);
    o = fail_entry(2, EMS_ACQ_RES_TIMEOUT, S(40));               /* failed probe */
    TEST_ASSERT_FALSE(o.device_offline);                         /* reported once */
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].offline_transitions);
    TEST_ASSERT_EQUAL_UINT8(3u, A.dev[0].consecutive_failures);
    TEST_ASSERT_EQUAL_UINT32(0u, ems_acq_core_tick(&A, S(1000)));  /* freshness leaves COMM_LOST */
    assert_all(0, EMS_Q_COMM_LOST);
}

static void test_DEV001_threshold_from_configuration(void)
{
    copy_cfg();
    C_dev[0].comm_lost_threshold = 1u;
    start();
    feed_all(S(1));
    TEST_ASSERT_TRUE(fail_entry(0, EMS_ACQ_RES_TIMEOUT, S(6)).device_offline);
    copy_cfg();
    C_dev[0].comm_lost_threshold = 3u;
    start();
    TEST_ASSERT_FALSE(fail_entry(0, EMS_ACQ_RES_TIMEOUT, S(1)).device_offline);
    TEST_ASSERT_FALSE(fail_entry(1, EMS_ACQ_RES_TIMEOUT, S(2)).device_offline);
    TEST_ASSERT_TRUE(fail_entry(2, EMS_ACQ_RES_TIMEOUT, S(3)).device_offline);
}

static void test_DEV002_probe_ok_brings_device_online_groups_recover_separately(void)
{
    start_lab();
    feed_all(S(1));
    (void)fail_entry(0, EMS_ACQ_RES_TIMEOUT, S(6));
    (void)fail_entry(1, EMS_ACQ_RES_TIMEOUT, S(9));
    ems_acq_outcome_t o;
    ems_acq_result_t r = ok_result(2, 5u);                       /* probe: status group */
    ems_acq_core_process(&A, &r, S(40), &o);
    TEST_ASSERT_TRUE(o.device_online);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_ONLINE, ems_acq_core_device_mode(&A, 0));
    TEST_ASSERT_EQUAL_UINT8(0u, o.consecutive_failures);
    assert_q(0, EMS_PARAM_DEV_STATUS_CODE, EMS_Q_GOOD);
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_COMM_LOST);      /* until its own read */
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_COMM_LOST);
    r = ok_result(0, 6u);
    ems_acq_core_process(&A, &r, S(40.2), &o);
    TEST_ASSERT_FALSE(o.device_online);                          /* reported once */
    assert_q(0, EMS_PARAM_AC_VOLTAGE_L1_N, EMS_Q_GOOD);
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_COMM_LOST);
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].online_transitions);
}

static void test_DEV002_exception_on_probe_also_brings_device_online(void)
{
    start_lab();
    (void)fail_entry(0, EMS_ACQ_RES_TIMEOUT, S(1));
    (void)fail_entry(1, EMS_ACQ_RES_TIMEOUT, S(2));
    ems_acq_outcome_t o;
    ems_acq_result_t r = status_result(2, EMS_ACQ_RES_EXCEPTION, 2u);
    ems_acq_core_process(&A, &r, S(32), &o);
    TEST_ASSERT_TRUE(o.device_online);
    assert_q(0, EMS_PARAM_DEV_STATUS_CODE, EMS_Q_INVALID);
}

static void test_DEV004_exceptions_never_take_device_offline(void)
{
    start_lab();
    feed_all(S(1));
    for (int i = 0; i < 5; i++) {
        ems_acq_outcome_t o;
        ems_acq_result_t r = status_result(1, EMS_ACQ_RES_EXCEPTION, 2u);
        ems_acq_core_process(&A, &r, S(10 + i), &o);
        TEST_ASSERT_FALSE(o.device_offline);
        o = fail_entry(0, EMS_ACQ_RES_TIMEOUT, S(10.5 + i));       /* 1 failure, then reset */
        TEST_ASSERT_FALSE(o.device_offline);
    }
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_ONLINE, ems_acq_core_device_mode(&A, 0));
    assert_q(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, EMS_Q_INVALID);
}

static void test_DEV006_crc_and_protocol_errors_count_like_timeouts(void)
{
    start_lab();
    feed_all(S(1));
    TEST_ASSERT_FALSE(fail_entry(0, EMS_ACQ_RES_CRC_ERROR, S(2)).device_offline);
    TEST_ASSERT_TRUE(fail_entry(1, EMS_ACQ_RES_PROTOCOL_ERROR, S(3)).device_offline);
    assert_all(0, EMS_Q_COMM_LOST);
}

static void test_cancelled_does_not_reset_failure_count(void)
{
    start_lab();
    (void)fail_entry(0, EMS_ACQ_RES_TIMEOUT, S(1));
    (void)fail_entry(1, EMS_ACQ_RES_CANCELLED, S(2));
    TEST_ASSERT_TRUE(fail_entry(2, EMS_ACQ_RES_TIMEOUT, S(3)).device_offline);
}

static void test_DEV005_disabled_device_mode(void)
{
    copy_cfg();
    C_dev[1] = C_dev[0];
    C_dev[1].slot = 1u;
    C_dev[1].slave_id = 2u;
    C_dev[1].enabled = false;
    C.device_count = 2u;
    start();
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_DISABLED, ems_acq_core_device_mode(&A, 1));
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_ONLINE, ems_acq_core_device_mode(&A, 0));
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_DISABLED, ems_acq_core_device_mode(&A, 3));   /* no such slot */
}

/* DIAG-001/002: scripted sequence of 10 OK, 2 timeouts, 1 exception. */
static void test_DIAG001_002_counters_match_script(void)
{
    start_lab();
    int64_t t = 0;
    for (int i = 0; i < 10; i++) {
        t += S(1);
        ems_acq_result_t r = ok_result((uint8_t)(i % 3), (uint32_t)i + 1u);
        ems_acq_core_process(&A, &r, t, NULL);
        if (i == 3 || i == 7) {
            (void)fail_entry(0, EMS_ACQ_RES_TIMEOUT, t + S(0.5));
        }
    }
    ems_acq_result_t ex = status_result(1, EMS_ACQ_RES_EXCEPTION, 3u);
    ems_acq_core_process(&A, &ex, t + S(0.7), NULL);
    const ems_acq_device_stats_t *d = &A.dev[0];
    TEST_ASSERT_EQUAL_UINT32(10u, d->ok);
    TEST_ASSERT_EQUAL_UINT32(2u, d->timeouts);
    TEST_ASSERT_EQUAL_UINT32(1u, d->exceptions);
    TEST_ASSERT_EQUAL_UINT8(3u, d->last_exception);
    TEST_ASSERT_EQUAL_UINT32(0u, d->offline_transitions);       /* never 2 in a row */
    TEST_ASSERT_EQUAL_UINT32(13u, A.stats.results);
    TEST_ASSERT_EQUAL_INT64(t, d->last_ok_us);                   /* DIAG-002 */
}


/* ESC-003 host part (D8): a bus fault takes an ONLINE device OFFLINE at once,
 * every parameter COMM_LOST with its value and time kept; cancelled results
 * afterwards change nothing; the first answer after BUS_OK brings it back. */
static void test_ESC003_bus_fault_forces_offline_comm_lost(void)
{
    start_lab();
    feed_all(S(1));
    TEST_ASSERT_TRUE(ems_acq_core_force_offline(&A, 0));
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_OFFLINE, ems_acq_core_device_mode(&A, 0));
    assert_all(0, EMS_Q_COMM_LOST);
    ASSERT_SAME_DOUBLE(12.5, rec(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL).value);
    TEST_ASSERT_EQUAL_INT64(S(1), rec(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL).sample_mono_us);
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].offline_transitions);
    TEST_ASSERT_FALSE(ems_acq_core_force_offline(&A, 0));        /* reported once */
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[0].offline_transitions);

    ems_acq_outcome_t o;
    ems_acq_result_t r = status_result(0, EMS_ACQ_RES_CANCELLED, 0u);   /* cancel_pending */
    ems_acq_core_process(&A, &r, S(2), &o);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_CANCELLED, o.kind);
    TEST_ASSERT_FALSE(o.device_online);
    assert_all(0, EMS_Q_COMM_LOST);

    r = ok_result(2, 9u);                                         /* probe after BUS_OK */
    ems_acq_core_process(&A, &r, S(60), &o);
    TEST_ASSERT_TRUE(o.device_online);
    assert_q(0, EMS_PARAM_DEV_STATUS_CODE, EMS_Q_GOOD);
}

static void test_force_offline_rejects_disabled_and_bad_slot(void)
{
    copy_cfg();
    C_dev[1] = C_dev[0];
    C_dev[1].slot = 1u;
    C_dev[1].slave_id = 2u;
    C_dev[1].enabled = false;
    C.device_count = 2u;
    start();
    TEST_ASSERT_FALSE(ems_acq_core_force_offline(&A, 1));         /* disabled */
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_DISABLED, ems_acq_core_device_mode(&A, 1));
    TEST_ASSERT_FALSE(ems_acq_core_force_offline(&A, 2));         /* no such device */
    TEST_ASSERT_FALSE(ems_acq_core_force_offline(NULL, 0));
    TEST_ASSERT_TRUE(ems_acq_core_force_offline(&A, 0));
}

void run_acq_core_tests(void)
{
    RUN_TEST(test_init_layout_and_Q001_initializing);
    RUN_TEST(test_UMD006_Q002_vectors_4_1_published_good);
    RUN_TEST(test_UMD007_export_and_CONV008_sign);
    RUN_TEST(test_UMD009_seq_increments_on_every_write);
    RUN_TEST(test_RTE002_each_group_updates_only_its_parameters);
    RUN_TEST(test_RTE003_wrong_register_count);
    RUN_TEST(test_RTE004_unknown_entry_and_wrong_slave);
    RUN_TEST(test_RTE005_two_devices_no_cross_talk);
    RUN_TEST(test_Q003_timeout_after_good_gives_stale_keeps_value);
    RUN_TEST(test_Q003_failure_before_first_sample_stays_initializing);
    RUN_TEST(test_Q004_freshness_turns_good_stale);
    RUN_TEST(test_Q005_exception_gives_invalid_and_resets_failures);
    RUN_TEST(test_Q006_out_of_range);
    RUN_TEST(test_Q007_recovery_after_stale);
    RUN_TEST(test_Q008_valid_zero_is_good);
    RUN_TEST(test_Q010_cancelled_changes_nothing);
    RUN_TEST(test_invalid_mapping_value_marks_only_that_parameter);
    RUN_TEST(test_CONV004_006_ct200_vectors_4_3_full_path);
    RUN_TEST(test_CONV005_meter_reports_primary_no_ratio);
    RUN_TEST(test_ENG_counters_through_full_path);
    RUN_TEST(test_ENG_out_of_range_counter_does_not_move_baseline);
    RUN_TEST(test_disabled_device_not_supported);
    RUN_TEST(test_DEV001_two_failures_take_device_offline);
    RUN_TEST(test_DEV001_threshold_from_configuration);
    RUN_TEST(test_DEV002_probe_ok_brings_device_online_groups_recover_separately);
    RUN_TEST(test_DEV002_exception_on_probe_also_brings_device_online);
    RUN_TEST(test_DEV004_exceptions_never_take_device_offline);
    RUN_TEST(test_DEV006_crc_and_protocol_errors_count_like_timeouts);
    RUN_TEST(test_cancelled_does_not_reset_failure_count);
    RUN_TEST(test_DEV005_disabled_device_mode);
    RUN_TEST(test_DIAG001_002_counters_match_script);
    RUN_TEST(test_ESC003_bus_fault_forces_offline_comm_lost);
    RUN_TEST(test_force_offline_rejects_disabled_and_bad_slot);
}
