/*
 * DP-WP5 M3: the DC meter through the full acquisition path on the host.
 *
 * Built into the site executables of lab_dc_bench and lab_mixed (see
 * CMakeLists.txt). Register words go in as completed FC04/FC03 results,
 * through request-ID routing, the generated DC map, decode/scale/range,
 * counters and the state store, exactly as in the firmware's acquisition
 * task (main/ems_acq_core.c). No DC-specific code exists in the firmware;
 * these tests show the generic path handles the DC data.
 *
 * Test IDs: DP-WP5 Test Specification v1.0, Section 7 (DCDEC-001..008) and
 * Section 2 (vectors 2.1 and edge vectors E1..E7). In the mixed site the AC
 * meter is fed the WP4 vectors at the same time (MIX-002/MIX-005 host part).
 */
#if defined(EMS_TEST_SITE_DC) || defined(EMS_TEST_SITE_MIXED)

#include <math.h>
#include <stdio.h>
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

static ems_acq_core_t   A;
static ems_sched_plan_t P;
static uint8_t          DC;           /* slot of the DC meter */

/* DC register image (Test Spec 2.1, NORMAL, import case) */
static uint16_t R_meas[6];            /* FC04 0..5     */
static uint16_t R_energy[4];          /* FC04 100..103 */
static uint16_t R_status[1];          /* FC03 200      */
/* AC image (WP4 Test Spec 4.1) for the mixed site */
static uint16_t R_ac[19];

static void image_2_1(void)
{
    static const uint16_t m[6] = { 0x4448, 0x0000,      /* 800.0 V FLOAT32 ABCD       */
                                   0xE848, 0x0001,      /* 125000 mA INT32 CDAB       */
                                   0x0000, 0x42C8 };    /* 100.0 kW FLOAT32 CDAB      */
    static const uint16_t e[4] = { 0xD090, 0x0003,      /* 250000 x 0.01 kWh CDAB     */
                                   0x0000, 0x0000 };    /* 0 kWh                      */
    static const uint16_t ac[19] = { 0x08FC, 0x0906, 0x08F7, 0x03E8, 0x03F2, 0x03DE, 0x0000, 0x30D4,
                                     0x0000, 0x0BB8, 0x0000, 0x3237, 0x03CC, 0x1388, 0x0000, 0x2729,
                                     0x0000, 0x0000, 0x0000 };
    memcpy(R_meas, m, sizeof(m));
    memcpy(R_energy, e, sizeof(e));
    R_status[0] = 0x0000;
    memcpy(R_ac, ac, sizeof(ac));
}

static void set_u32_cdab(uint16_t *w, uint32_t v)
{
    w[0] = (uint16_t)(v & 0xFFFFu);
    w[1] = (uint16_t)(v >> 16);
}

static void start(void)
{
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_OK, ems_sched_plan_build(&ems_config_build, &P));
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_INIT_OK, ems_acq_core_init(&A, &ems_config_build, &P, NULL));
    DC = 0xFFu;
    for (uint8_t d = 0; d < ems_config_build.device_count; d++) {
        const ems_cfg_profile_t *p = &ems_config_build.profiles[ems_config_build.devices[d].profile_index];
        if (strcmp(p->device_type, "DC_METER") == 0) {
            DC = d;
        }
    }
    TEST_ASSERT_NOT_EQUAL(0xFF, DC);
    image_2_1();
}

static const uint16_t *image_for(const ems_sched_entry_t *en)
{
    if (en->slot != DC) {
        return &R_ac[en->start_address];
    }
    switch (en->start_address) {
    case 0u:   return R_meas;
    case 100u: return R_energy;
    case 200u: return R_status;
    default:   TEST_FAIL_MESSAGE("unexpected DC poll group"); return NULL;
    }
}

static ems_acq_result_t result_for(uint8_t e, uint32_t seq)
{
    const ems_sched_entry_t *en = &P.entries[e];
    ems_acq_result_t r = {
        .registers = image_for(en), .request_id = ems_sched_make_request_id(e, seq),
        .register_count = en->count, .slave_addr = en->slave_id, .function_code = en->function_code,
        .status = EMS_ACQ_RES_OK, .exception_code = 0u };
    return r;
}

/* Entry of a DC poll group by its start address. */
static uint8_t dc_entry(uint16_t start)
{
    for (uint8_t e = 0; e < P.entry_count; e++) {
        if (P.entries[e].slot == DC && P.entries[e].start_address == start) {
            return e;
        }
    }
    TEST_FAIL_MESSAGE("DC poll group not in plan");
    return 0u;
}

static ems_acq_outcome_t feed(uint8_t e, int64_t t)
{
    ems_acq_outcome_t o;
    ems_acq_result_t r = result_for(e, 1u);
    ems_acq_core_process(&A, &r, t, &o);
    return o;
}

static void feed_all(int64_t t)
{
    for (uint8_t e = 0; e < P.entry_count; e++) {
        ems_acq_outcome_t o = feed(e, t);
        TEST_ASSERT_EQUAL_STRING(ems_acq_outcome_name(EMS_ACQ_OUT_OK), ems_acq_outcome_name((ems_acq_outcome_kind_t)o.kind));
    }
}

static ems_measurement_record_t rec(uint8_t slot, uint16_t id)
{
    ems_measurement_record_t r;
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_get(slot, id, &r));
    return r;
}

static void assert_good(uint16_t id, double v)
{
    ems_measurement_record_t r = rec(DC, id);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(ems_quality_name(EMS_Q_GOOD), ems_quality_name((ems_quality_t)r.quality),
                                     ems_param_find(id)->name);
    if (dbits(v) != dbits(r.value)) {
        char msg[96];
        (void)snprintf(msg, sizeof(msg), "%s: expected %.17g, got %.17g", ems_param_find(id)->name, v, r.value);
        TEST_FAIL_MESSAGE(msg);
    }
}

static void assert_q(uint16_t id, ems_quality_t q)
{
    TEST_ASSERT_EQUAL_STRING_MESSAGE(ems_quality_name(q), ems_quality_name((ems_quality_t)rec(DC, id).quality),
                                     ems_param_find(id)->name);
}

/* DCDEC-001, DCDEC-005 (energy group at 100), DCDEC-008 (FC04 routed) */
static void test_DCDEC001_normal_vectors_full_path(void)
{
    start();
    feed_all(S(10));
    assert_good(EMS_PARAM_DC_VOLTAGE, 800.0);
    assert_good(EMS_PARAM_DC_CURRENT, 125.0);
    assert_good(EMS_PARAM_DC_POWER, 100.0);
    assert_good(EMS_PARAM_DC_ENERGY_IMPORT, 2500.0);
    assert_good(EMS_PARAM_DC_ENERGY_EXPORT, 0.0);
    assert_good(EMS_PARAM_DEV_STATUS_CODE, 0.0);
    TEST_ASSERT_EQUAL_INT64(S(10), rec(DC, EMS_PARAM_DC_POWER).sample_mono_us);
    TEST_ASSERT_EQUAL_UINT32(0u, A.stats.routing_errors);
    TEST_ASSERT_EQUAL_UINT32(3u, A.dev[DC].ok);
}

/* DCDEC-008: the routing check compares the function code of the entry */
static void test_DCDEC008_fc04_routing(void)
{
    start();
    const uint8_t e = dc_entry(0u);
    TEST_ASSERT_EQUAL_UINT8(4u, P.entries[e].function_code);
    TEST_ASSERT_EQUAL_UINT8(4u, P.entries[dc_entry(100u)].function_code);
    TEST_ASSERT_EQUAL_UINT8(3u, P.entries[dc_entry(200u)].function_code);
    ems_acq_outcome_t o;
    ems_acq_result_t r = result_for(e, 2u);
    r.function_code = 3u;                                  /* FC03 answer for an FC04 entry */
    ems_acq_core_process(&A, &r, S(1), &o);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_ROUTING_ERROR, o.kind);
    TEST_ASSERT_EQUAL_UINT32(1u, A.stats.routing_errors);
    assert_q(EMS_PARAM_DC_VOLTAGE, EMS_Q_INITIALIZING);
    o = feed(e, S(2));
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_OUT_OK, o.kind);
    TEST_ASSERT_EQUAL_UINT8(3u, o.good);
}

/* DCDEC-002: export vectors; DCDEC-004: no ratio applied (values = scaled raw) */
static void test_DCDEC002_export_sign(void)
{
    start();
    R_meas[2] = 0x17B8; R_meas[3] = 0xFFFE;              /* -125000 mA */
    R_meas[4] = 0x0000; R_meas[5] = 0xC2C8;              /* -100.0 kW  */
    feed(dc_entry(0u), S(1));
    assert_good(EMS_PARAM_DC_CURRENT, -125.0);
    assert_good(EMS_PARAM_DC_POWER, -100.0);
    assert_good(EMS_PARAM_DC_VOLTAGE, 800.0);
    TEST_ASSERT_FALSE(A.apply_ratio[DC]);                 /* METER_REPORTS_PRIMARY */
}

/* DCDEC-003: edge vectors E1..E7 */
static void test_DCDEC003_edge_vectors(void)
{
    start();
    const uint8_t e = dc_entry(0u);
    ems_acq_outcome_t o;

    /* E1 current 0 is a valid zero; E4 voltage at max; E7 power at max */
    R_meas[2] = 0x0000; R_meas[3] = 0x0000;
    R_meas[0] = 0x44BB; R_meas[1] = 0x8000;
    R_meas[4] = 0x8000; R_meas[5] = 0x453B;
    o = feed(e, S(1));
    TEST_ASSERT_EQUAL_UINT8(3u, o.good);
    assert_good(EMS_PARAM_DC_CURRENT, 0.0);
    assert_good(EMS_PARAM_DC_VOLTAGE, 1500.0);
    assert_good(EMS_PARAM_DC_POWER, 3000.0);

    /* E2 current at min (inclusive) */
    R_meas[2] = 0x7B80; R_meas[3] = 0xFFE1;
    feed(e, S(2));
    assert_good(EMS_PARAM_DC_CURRENT, -2000.0);

    /* E3 one count below min; E5 voltage 1800 (sim_out_of_range) */
    R_meas[2] = 0x7B7F; R_meas[3] = 0xFFE1;
    R_meas[0] = 0x44E1; R_meas[1] = 0x0000;
    o = feed(e, S(3));
    TEST_ASSERT_EQUAL_UINT8(2u, o.out_of_range);
    assert_q(EMS_PARAM_DC_CURRENT, EMS_Q_OUT_OF_RANGE);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, -2000.001, rec(DC, EMS_PARAM_DC_CURRENT).value);   /* kept for diagnostics */
    assert_q(EMS_PARAM_DC_VOLTAGE, EMS_Q_OUT_OF_RANGE);
    assert_good(EMS_PARAM_DC_POWER, 3000.0);

    /* E6 power NaN -> INVALID, the last GOOD value is not overwritten by NaN */
    R_meas[4] = 0x0000; R_meas[5] = 0x7FC0;
    o = feed(e, S(4));
    TEST_ASSERT_EQUAL_UINT8(1u, o.invalid);
    assert_q(EMS_PARAM_DC_POWER, EMS_Q_INVALID);
    TEST_ASSERT_FALSE(isnan(rec(DC, EMS_PARAM_DC_POWER).value));
}

/* DCDEC-006: import counter delta; DCDEC-007: reset -> discontinuity */
static void test_DCDEC006_007_counters(void)
{
    start();
    const uint8_t e = dc_entry(100u);
    feed(e, S(0));
    set_u32_cdab(&R_energy[0], 250028u);                  /* 2500.28 kWh */
    ems_acq_outcome_t o = feed(e, S(10));
    TEST_ASSERT_EQUAL_UINT8(0u, o.counter_discontinuities);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.28, ems_acq_core_counter_delta(&A, DC, EMS_PARAM_DC_ENERGY_IMPORT));
    assert_good(EMS_PARAM_DC_ENERGY_IMPORT, 2500.28);

    set_u32_cdab(&R_energy[0], 0u);                       /* meter reset */
    o = feed(e, S(20));
    TEST_ASSERT_EQUAL_UINT8(1u, o.counter_discontinuities);
    TEST_ASSERT_EQUAL_HEX16(EMS_PARAM_DC_ENERGY_IMPORT, o.discontinuity_param);
    assert_good(EMS_PARAM_DC_ENERGY_IMPORT, 0.0);
    ASSERT_SAME_DOUBLE(0.0, ems_acq_core_counter_delta(&A, DC, EMS_PARAM_DC_ENERGY_IMPORT));
    TEST_ASSERT_EQUAL_UINT32(1u, A.dev[DC].counter_discontinuities);

    set_u32_cdab(&R_energy[0], 25u);                      /* deltas from the new baseline */
    feed(e, S(30));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.25, ems_acq_core_counter_delta(&A, DC, EMS_PARAM_DC_ENERGY_IMPORT));
    /* export counter, while the import counter stays */
    set_u32_cdab(&R_energy[2], 17u);
    feed(e, S(40));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.17, ems_acq_core_counter_delta(&A, DC, EMS_PARAM_DC_ENERGY_EXPORT));
    ASSERT_SAME_DOUBLE(3000.0, A.counter_max_rate[DC]);
}

/* Failure is never zero (WP4 DR-06) for a DC meter: TIMEOUT -> STALE, then OFFLINE -> COMM_LOST */
static void test_dc_timeout_never_zero(void)
{
    start();
    feed_all(S(1));
    const uint8_t e = dc_entry(0u);
    ems_acq_outcome_t o;
    ems_acq_result_t r = result_for(e, 5u);
    r.status = EMS_ACQ_RES_TIMEOUT; r.registers = NULL; r.register_count = 0u;
    ems_acq_core_process(&A, &r, S(5), &o);
    assert_q(EMS_PARAM_DC_CURRENT, EMS_Q_STALE);
    ASSERT_SAME_DOUBLE(125.0, rec(DC, EMS_PARAM_DC_CURRENT).value);
    ems_acq_core_process(&A, &r, S(9), &o);
    TEST_ASSERT_TRUE(o.device_offline);
    assert_q(EMS_PARAM_DC_CURRENT, EMS_Q_COMM_LOST);
    assert_q(EMS_PARAM_DC_ENERGY_IMPORT, EMS_Q_COMM_LOST);
    ASSERT_SAME_DOUBLE(125.0, rec(DC, EMS_PARAM_DC_CURRENT).value);
    TEST_ASSERT_EQUAL_INT64(S(1), rec(DC, EMS_PARAM_DC_CURRENT).sample_mono_us);
}

/* Freshness: measurements stale timeout 9 s (Design 6.2) */
static void test_dc_stale_timeout(void)
{
    start();
    feed_all(S(1));
    (void)ems_acq_core_tick(&A, S(9.9));
    assert_q(EMS_PARAM_DC_VOLTAGE, EMS_Q_GOOD);
    (void)ems_acq_core_tick(&A, S(10.1));
    assert_q(EMS_PARAM_DC_VOLTAGE, EMS_Q_STALE);
    assert_q(EMS_PARAM_DC_ENERGY_IMPORT, EMS_Q_GOOD);    /* 21 s */
}

#if defined(EMS_TEST_SITE_MIXED)
/* MIX-002 / MIX-005 host part: AC and DC on one bus, independent records and signs */
static void test_mixed_independent_devices(void)
{
    start();
    R_ac[6] = 0xFFFF; R_ac[7] = 0xCF2C; R_ac[12] = 0xFC34;   /* AC export -12.5 kW */
    feed_all(S(1));
    TEST_ASSERT_EQUAL_UINT8(0u, ems_config_build.devices[0].slot);
    ems_measurement_record_t ac = rec(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL);
    ASSERT_SAME_DOUBLE(-12.5, ac.value);
    assert_good(EMS_PARAM_DC_POWER, 100.0);
    /* a DC parameter is not part of the AC device and the other way round */
    ems_measurement_record_t x;
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_get(0, EMS_PARAM_DC_POWER, &x));
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_NOT_SUPPORTED, x.quality);
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_get(DC, EMS_PARAM_AC_POWER_ACTIVE_TOTAL, &x));
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_NOT_SUPPORTED, x.quality);
    /* status is shared by both: each slot has its own record */
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_get(0, EMS_PARAM_DEV_STATUS_CODE, &x));
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_get(DC, EMS_PARAM_DEV_STATUS_CODE, &x));
    /* AC offline does not touch the DC meter */
    ems_acq_outcome_t o;
    ems_acq_result_t r = result_for(0u, 9u);
    r.status = EMS_ACQ_RES_TIMEOUT; r.registers = NULL; r.register_count = 0u;
    ems_acq_core_process(&A, &r, S(2), &o);
    ems_acq_core_process(&A, &r, S(3), &o);
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_OFFLINE, ems_acq_core_device_mode(&A, 0u));
    TEST_ASSERT_EQUAL_INT(EMS_ACQ_DEV_ONLINE, ems_acq_core_device_mode(&A, DC));
    assert_good(EMS_PARAM_DC_POWER, 100.0);
    ASSERT_SAME_DOUBLE(1000.0, A.counter_max_rate[0]);        /* AC limit unchanged next to the DC one */
}
#endif

void run_dc_meter_tests(void)
{
    RUN_TEST(test_DCDEC001_normal_vectors_full_path);
    RUN_TEST(test_DCDEC008_fc04_routing);
    RUN_TEST(test_DCDEC002_export_sign);
    RUN_TEST(test_DCDEC003_edge_vectors);
    RUN_TEST(test_DCDEC006_007_counters);
    RUN_TEST(test_dc_timeout_never_zero);
    RUN_TEST(test_dc_stale_timeout);
#if defined(EMS_TEST_SITE_MIXED)
    RUN_TEST(test_mixed_independent_devices);
#endif
}

#else
typedef int ems_test_dc_meter_unused_t;   /* ISO C: no empty translation unit */
#endif
