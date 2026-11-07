/*
 * State store tests (DP-WP4 4B, Design 6.2-6.4).
 * Covers the host parts of Q-001..Q-008, UMD-005, UMD-008, UMD-009 and the
 * init/argument rules of ems_state_store.h.
 */
#include <math.h>
#include <string.h>

#include "unity.h"
#include "ems_state_store.h"

/* ------------------------------------------------------------ fixtures --- */

#define MS(x) ((int64_t)(x) * 1000)        /* ms -> us */
#define T0    MS(100000)                   /* arbitrary start time */

/* Device 0: AC meter subset, timeouts from the WP4 poll groups (Design 8.2). */
static const ems_state_param_def_t k_meter_params[] = {
    { EMS_PARAM_AC_VOLTAGE_L1_N,         11000u },  /* electrical */
    { EMS_PARAM_AC_POWER_ACTIVE_TOTAL,   11000u },  /* electrical */
    { EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, 21000u },  /* energy     */
    { EMS_PARAM_DEV_STATUS_CODE,         61000u },  /* status     */
};
/* Device 1: disabled in the site file. */
static const ems_state_param_def_t k_disabled_params[] = {
    { EMS_PARAM_AC_VOLTAGE_L1_N, 11000u },
};
static const ems_state_device_def_t k_devices[] = {
    { k_meter_params, 4u, true },
    { k_disabled_params, 1u, false },
};
static const ems_state_layout_t k_layout = { k_devices, 2u };

static ems_measurement_record_t get(uint8_t slot, uint16_t param)
{
    ems_measurement_record_t r;
    memset(&r, 0xA5, sizeof(r));
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_get(slot, param, &r));
    return r;
}

static void init_default(void)
{
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_init(&k_layout, NULL));
}

/* ----------------------------------------------------------- Q-tests --- */

static void test_Q001_before_first_sample_is_initializing(void)
{
    init_default();
    const ems_measurement_record_t r = get(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_INITIALIZING, r.quality);
    TEST_ASSERT_EQUAL_INT64(0, r.sample_mono_us);
    TEST_ASSERT_EQUAL_UINT32(0u, r.seq);
    TEST_ASSERT_EQUAL_HEX16(EMS_PARAM_AC_VOLTAGE_L1_N, r.param_id);
    TEST_ASSERT_EQUAL_UINT8(0u, r.device_slot);
}

static void test_Q002_UMD008_first_valid_sample_is_good_with_timestamp(void)
{
    init_default();
    TEST_ASSERT_EQUAL_INT(EMS_OK,
        ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 230.0, EMS_Q_GOOD, T0));
    const ems_measurement_record_t r = get(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_GOOD, r.quality);
    TEST_ASSERT_EQUAL_DOUBLE(230.0, r.value);
    TEST_ASSERT_EQUAL_INT64(T0, r.sample_mono_us);
    TEST_ASSERT_EQUAL_UINT8(0u, r.utc_valid);          /* no time service in WP4 */
    TEST_ASSERT_EQUAL_INT64(0, r.sample_utc_ms);
    TEST_ASSERT_EQUAL_UINT32(1u, r.seq);
}

static void test_Q003_failure_after_good_is_stale_and_keeps_value_and_time(void)
{
    init_default();
    ems_state_publish(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL, 12.5, EMS_Q_GOOD, T0);
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_mark_failed(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL));
    const ems_measurement_record_t r = get(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_STALE, r.quality);
    TEST_ASSERT_EQUAL_DOUBLE(12.5, r.value);
    TEST_ASSERT_EQUAL_INT64(T0, r.sample_mono_us);
}

static void test_Q003_failure_does_not_change_other_qualities(void)
{
    init_default();
    /* INITIALIZING stays INITIALIZING */
    ems_state_mark_failed(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_INITIALIZING, get(0, EMS_PARAM_AC_VOLTAGE_L1_N).quality);
    /* INVALID stays INVALID */
    ems_state_mark_invalid(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL);
    ems_state_mark_failed(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_INVALID, get(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL).quality);
    /* OUT_OF_RANGE stays OUT_OF_RANGE */
    ems_state_publish(0, EMS_PARAM_DEV_STATUS_CODE, 9.0, EMS_Q_OUT_OF_RANGE, T0);
    ems_state_mark_failed(0, EMS_PARAM_DEV_STATUS_CODE);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_OUT_OF_RANGE, get(0, EMS_PARAM_DEV_STATUS_CODE).quality);
}

static void test_Q004_freshness_turns_over_age_good_into_stale(void)
{
    init_default();
    ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 230.0, EMS_Q_GOOD, T0);       /* 11 s */
    ems_state_publish(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, 100.25, EMS_Q_GOOD, T0); /* 21 s */

    /* Exactly at the timeout: still GOOD (age <= stale timeout). */
    TEST_ASSERT_EQUAL_UINT32(0u, ems_state_check_freshness(T0 + MS(11000)));
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_GOOD, get(0, EMS_PARAM_AC_VOLTAGE_L1_N).quality);

    /* 1 us later: voltage STALE, energy (21 s) still GOOD. */
    TEST_ASSERT_EQUAL_UINT32(1u, ems_state_check_freshness(T0 + MS(11000) + 1));
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_STALE, get(0, EMS_PARAM_AC_VOLTAGE_L1_N).quality);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_GOOD, get(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE).quality);

    /* Already STALE is not counted again; energy goes STALE after 21 s. */
    TEST_ASSERT_EQUAL_UINT32(1u, ems_state_check_freshness(T0 + MS(21000) + 1));
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_STALE, get(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE).quality);
    TEST_ASSERT_EQUAL_DOUBLE(100.25, get(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE).value);
    TEST_ASSERT_EQUAL_UINT32(0u, ems_state_check_freshness(T0 + MS(30000)));
}

static void test_Q004_clock_going_backwards_does_not_mark_stale(void)
{
    init_default();
    ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 230.0, EMS_Q_GOOD, T0);
    TEST_ASSERT_EQUAL_UINT32(0u, ems_state_check_freshness(T0 - MS(50000)));
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_GOOD, get(0, EMS_PARAM_AC_VOLTAGE_L1_N).quality);
}

static void test_Q005_invalid_keeps_value_and_time(void)
{
    init_default();
    ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 230.0, EMS_Q_GOOD, T0);
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_mark_invalid(0, EMS_PARAM_AC_VOLTAGE_L1_N));
    const ems_measurement_record_t r = get(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_INVALID, r.quality);
    TEST_ASSERT_EQUAL_INT64(T0, r.sample_mono_us);
    TEST_ASSERT_FALSE(ems_quality_is_usable((ems_quality_t)r.quality));
}

static void test_Q006_out_of_range_stores_value_but_not_sample_time(void)
{
    init_default();
    ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 230.0, EMS_Q_GOOD, T0);
    TEST_ASSERT_EQUAL_INT(EMS_OK,
        ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 612.0, EMS_Q_OUT_OF_RANGE, T0 + MS(5000)));
    const ems_measurement_record_t r = get(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_OUT_OF_RANGE, r.quality);
    TEST_ASSERT_EQUAL_DOUBLE(612.0, r.value);           /* for diagnostics */
    TEST_ASSERT_EQUAL_INT64(T0, r.sample_mono_us);      /* last GOOD sample */
    TEST_ASSERT_FALSE(ems_quality_is_usable((ems_quality_t)r.quality));
}

static void test_Q007_recovery_after_stale_returns_good_with_new_time(void)
{
    init_default();
    ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 230.0, EMS_Q_GOOD, T0);
    ems_state_mark_failed(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 231.0, EMS_Q_GOOD, T0 + MS(5000));
    const ems_measurement_record_t r = get(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_GOOD, r.quality);
    TEST_ASSERT_EQUAL_DOUBLE(231.0, r.value);
    TEST_ASSERT_EQUAL_INT64(T0 + MS(5000), r.sample_mono_us);
}

static void test_Q008_valid_zero_is_good(void)
{
    init_default();
    TEST_ASSERT_EQUAL_INT(EMS_OK,
        ems_state_publish(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL, 0.0, EMS_Q_GOOD, T0));
    const ems_measurement_record_t r = get(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_GOOD, r.quality);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, r.value);
}

static void test_negative_values_are_stored_exactly(void)
{
    init_default();
    ems_state_publish(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL, -12.5, EMS_Q_GOOD, T0);
    TEST_ASSERT_EQUAL_DOUBLE(-12.5, get(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL).value);
    /* 42,949,672.95 kWh (UINT32 max x 0.01) must survive exactly in double. */
    ems_state_publish(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE, 42949672.95, EMS_Q_GOOD, T0);
    TEST_ASSERT_EQUAL_DOUBLE(42949672.95, get(0, EMS_PARAM_AC_ENERGY_IMPORT_ACTIVE).value);
}

/* ------------------------------------------------ COMM_LOST / disabled --- */

static void test_device_comm_lost_marks_every_parameter_and_keeps_values(void)
{
    init_default();
    ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 230.0, EMS_Q_GOOD, T0);
    ems_state_publish(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL, 12.5, EMS_Q_GOOD, T0);
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_set_device_comm_lost(0));

    ems_measurement_record_t recs[EMS_MAX_PARAMS_PER_DEVICE];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_snapshot_device(0, recs, EMS_MAX_PARAMS_PER_DEVICE, &n));
    TEST_ASSERT_EQUAL_UINT32(4u, n);
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_EQUAL_UINT8(EMS_Q_COMM_LOST, recs[i].quality);
    }
    TEST_ASSERT_EQUAL_DOUBLE(230.0, get(0, EMS_PARAM_AC_VOLTAGE_L1_N).value);
    TEST_ASSERT_EQUAL_INT64(T0, get(0, EMS_PARAM_AC_VOLTAGE_L1_N).sample_mono_us);

    /* Back ONLINE: only the group that is read again becomes GOOD. */
    ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 229.0, EMS_Q_GOOD, T0 + MS(30000));
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_GOOD, get(0, EMS_PARAM_AC_VOLTAGE_L1_N).quality);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_COMM_LOST, get(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL).quality);
    /* COMM_LOST is not turned into STALE by the freshness check. */
    TEST_ASSERT_EQUAL_UINT32(1u, ems_state_check_freshness(T0 + MS(60000)));
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_COMM_LOST, get(0, EMS_PARAM_AC_POWER_ACTIVE_TOTAL).quality);
}

static void test_UMD005_undeclared_parameter_reads_as_not_supported(void)
{
    init_default();
    const ems_measurement_record_t r = get(0, EMS_PARAM_AC_FREQUENCY);
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_NOT_SUPPORTED, r.quality);
    TEST_ASSERT_EQUAL_HEX16(EMS_PARAM_AC_FREQUENCY, r.param_id);
    TEST_ASSERT_EQUAL_UINT32(0u, r.seq);
    TEST_ASSERT_EQUAL_INT64(0, r.sample_mono_us);
    /* Writing an undeclared parameter is refused. */
    TEST_ASSERT_EQUAL_INT(EMS_ERR_NOT_FOUND,
        ems_state_publish(0, EMS_PARAM_AC_FREQUENCY, 50.0, EMS_Q_GOOD, T0));
}

static void test_UMD005_disabled_device_is_not_supported_and_read_only(void)
{
    init_default();
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_NOT_SUPPORTED, get(1, EMS_PARAM_AC_VOLTAGE_L1_N).quality);
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_STATE,
        ems_state_publish(1, EMS_PARAM_AC_VOLTAGE_L1_N, 230.0, EMS_Q_GOOD, T0));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_STATE, ems_state_mark_failed(1, EMS_PARAM_AC_VOLTAGE_L1_N));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_STATE, ems_state_set_device_comm_lost(1));
    TEST_ASSERT_EQUAL_UINT8(EMS_Q_NOT_SUPPORTED, get(1, EMS_PARAM_AC_VOLTAGE_L1_N).quality);
}

/* ------------------------------------------------------------- UMD-009 --- */

static void test_UMD009_seq_increments_on_every_write_and_quality_change(void)
{
    init_default();
    const uint16_t p = EMS_PARAM_AC_VOLTAGE_L1_N;
    ems_state_publish(0, p, 230.0, EMS_Q_GOOD, T0);
    TEST_ASSERT_EQUAL_UINT32(1u, get(0, p).seq);
    ems_state_publish(0, p, 230.0, EMS_Q_GOOD, T0 + 1);   /* same value, new sample */
    TEST_ASSERT_EQUAL_UINT32(2u, get(0, p).seq);
    ems_state_mark_failed(0, p);                           /* GOOD -> STALE */
    TEST_ASSERT_EQUAL_UINT32(3u, get(0, p).seq);
    ems_state_mark_failed(0, p);                           /* no change */
    TEST_ASSERT_EQUAL_UINT32(3u, get(0, p).seq);
    ems_state_set_device_comm_lost(0);                     /* STALE -> COMM_LOST */
    TEST_ASSERT_EQUAL_UINT32(4u, get(0, p).seq);
    ems_state_set_device_comm_lost(0);                     /* no change */
    TEST_ASSERT_EQUAL_UINT32(4u, get(0, p).seq);
    ems_state_mark_invalid(0, p);
    TEST_ASSERT_EQUAL_UINT32(5u, get(0, p).seq);
}

/* --------------------------------------------------- argument checks --- */

static void test_publish_rejects_non_finite_values_and_wrong_quality(void)
{
    init_default();
    const uint16_t p = EMS_PARAM_AC_VOLTAGE_L1_N;
    ems_state_publish(0, p, 230.0, EMS_Q_GOOD, T0);
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_publish(0, p, (double)NAN, EMS_Q_GOOD, T0 + 1));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_publish(0, p, (double)INFINITY, EMS_Q_GOOD, T0 + 1));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_publish(0, p, -(double)INFINITY, EMS_Q_OUT_OF_RANGE, T0 + 1));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_publish(0, p, 1.0, EMS_Q_STALE, T0 + 1));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_publish(0, p, 1.0, EMS_Q_COMM_LOST, T0 + 1));
    /* Record untouched by the rejected calls. */
    const ems_measurement_record_t r = get(0, p);
    TEST_ASSERT_EQUAL_DOUBLE(230.0, r.value);
    TEST_ASSERT_EQUAL_UINT32(1u, r.seq);
}

static void test_bad_slot_unknown_parameter_and_null_pointers(void)
{
    init_default();
    ems_measurement_record_t r;
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_get(2, EMS_PARAM_AC_VOLTAGE_L1_N, &r));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_get(0, EMS_PARAM_AC_VOLTAGE_L1_N, NULL));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_NOT_FOUND, ems_state_get(0, 0x0199u, &r));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_publish(9, EMS_PARAM_AC_VOLTAGE_L1_N, 1.0, EMS_Q_GOOD, T0));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_NOT_FOUND, ems_state_publish(0, 0x0199u, 1.0, EMS_Q_GOOD, T0));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_set_device_comm_lost(9));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_snapshot_device(0, NULL, 4, &n));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_snapshot_device(0, &r, 1, NULL));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_NO_SPACE, ems_state_snapshot_device(0, &r, 1, &n));
    TEST_ASSERT_EQUAL_UINT32(0u, n);
}

static void test_calls_before_init_are_refused(void)
{
    ems_measurement_record_t r;
    size_t n = 0;
    TEST_ASSERT_FALSE(ems_state_is_initialised());
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_STATE, ems_state_get(0, EMS_PARAM_AC_VOLTAGE_L1_N, &r));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_STATE, ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 1.0, EMS_Q_GOOD, T0));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_STATE, ems_state_mark_failed(0, EMS_PARAM_AC_VOLTAGE_L1_N));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_STATE, ems_state_set_device_comm_lost(0));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_STATE, ems_state_snapshot_device(0, &r, 1, &n));
    TEST_ASSERT_EQUAL_UINT32(0u, ems_state_check_freshness(T0));
}

/* ------------------------------------------------------ init validation --- */

static void test_init_validates_layout(void)
{
    /* NULL / empty */
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_init(NULL, NULL));
    const ems_state_layout_t empty = { k_devices, 0u };
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_init(&empty, NULL));

    /* More devices than EMS_MAX_DEVICES */
    ems_state_device_def_t many[EMS_MAX_DEVICES + 1u];
    for (size_t i = 0; i < EMS_MAX_DEVICES + 1u; ++i) {
        many[i] = k_devices[0];
    }
    const ems_state_layout_t too_many = { many, (uint8_t)(EMS_MAX_DEVICES + 1u) };
    TEST_ASSERT_EQUAL_INT(EMS_ERR_NO_SPACE, ems_state_init(&too_many, NULL));

    /* More parameters than EMS_MAX_PARAMS_PER_DEVICE */
    const ems_state_device_def_t big = { k_meter_params, (uint8_t)(EMS_MAX_PARAMS_PER_DEVICE + 1u), true };
    const ems_state_layout_t too_big = { &big, 1u };
    TEST_ASSERT_EQUAL_INT(EMS_ERR_NO_SPACE, ems_state_init(&too_big, NULL));

    /* Duplicate parameter */
    const ems_state_param_def_t dup[] = { { EMS_PARAM_AC_FREQUENCY, 11000u }, { EMS_PARAM_AC_FREQUENCY, 11000u } };
    const ems_state_device_def_t dup_dev = { dup, 2u, true };
    const ems_state_layout_t dup_layout = { &dup_dev, 1u };
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_init(&dup_layout, NULL));

    /* Parameter not in the dictionary */
    const ems_state_param_def_t unknown[] = { { 0x0199u, 11000u } };
    const ems_state_device_def_t unknown_dev = { unknown, 1u, true };
    const ems_state_layout_t unknown_layout = { &unknown_dev, 1u };
    TEST_ASSERT_EQUAL_INT(EMS_ERR_NOT_FOUND, ems_state_init(&unknown_layout, NULL));

    /* Zero stale timeout */
    const ems_state_param_def_t zero[] = { { EMS_PARAM_AC_FREQUENCY, 0u } };
    const ems_state_device_def_t zero_dev = { zero, 1u, true };
    const ems_state_layout_t zero_layout = { &zero_dev, 1u };
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_ARG, ems_state_init(&zero_layout, NULL));

    /* Failed inits leave the store uninitialised */
    TEST_ASSERT_FALSE(ems_state_is_initialised());

    /* Valid, then a second init is refused until deinit */
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_init(&k_layout, NULL));
    TEST_ASSERT_EQUAL_INT(EMS_ERR_INVALID_STATE, ems_state_init(&k_layout, NULL));
    ems_state_deinit();
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_init(&k_layout, NULL));
}

static void test_snapshot_returns_records_in_layout_order(void)
{
    init_default();
    ems_measurement_record_t recs[8];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_snapshot_device(0, recs, 8, &n));
    TEST_ASSERT_EQUAL_UINT32(4u, n);
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_EQUAL_HEX16(k_meter_params[i].param_id, recs[i].param_id);
        TEST_ASSERT_EQUAL_UINT8(0u, recs[i].device_slot);
    }
}

/* -------------------------------------------------------------- locking --- */

typedef struct {
    int depth;
    int max_depth;
    int locks;
} lock_probe_t;

static void probe_lock(void *ctx)
{
    lock_probe_t *p = (lock_probe_t *)ctx;
    p->depth++;
    p->locks++;
    if (p->depth > p->max_depth) {
        p->max_depth = p->depth;
    }
}

static void probe_unlock(void *ctx)
{
    ((lock_probe_t *)ctx)->depth--;
}

static void test_lock_is_taken_balanced_and_never_nested(void)
{
    lock_probe_t probe = { 0, 0, 0 };
    const ems_state_lock_t lk = { probe_lock, probe_unlock, &probe };
    TEST_ASSERT_EQUAL_INT(EMS_OK, ems_state_init(&k_layout, &lk));
    ems_measurement_record_t recs[8];
    size_t n = 0;
    ems_state_publish(0, EMS_PARAM_AC_VOLTAGE_L1_N, 230.0, EMS_Q_GOOD, T0);
    ems_state_mark_failed(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    ems_state_mark_invalid(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    ems_state_set_device_comm_lost(0);
    (void)ems_state_check_freshness(T0);
    (void)get(0, EMS_PARAM_AC_VOLTAGE_L1_N);
    ems_state_snapshot_device(0, recs, 8, &n);
    TEST_ASSERT_EQUAL_INT(7, probe.locks);
    TEST_ASSERT_EQUAL_INT(0, probe.depth);
    TEST_ASSERT_EQUAL_INT(1, probe.max_depth);
}

void run_state_store_tests(void)
{
    RUN_TEST(test_Q001_before_first_sample_is_initializing);
    RUN_TEST(test_Q002_UMD008_first_valid_sample_is_good_with_timestamp);
    RUN_TEST(test_Q003_failure_after_good_is_stale_and_keeps_value_and_time);
    RUN_TEST(test_Q003_failure_does_not_change_other_qualities);
    RUN_TEST(test_Q004_freshness_turns_over_age_good_into_stale);
    RUN_TEST(test_Q004_clock_going_backwards_does_not_mark_stale);
    RUN_TEST(test_Q005_invalid_keeps_value_and_time);
    RUN_TEST(test_Q006_out_of_range_stores_value_but_not_sample_time);
    RUN_TEST(test_Q007_recovery_after_stale_returns_good_with_new_time);
    RUN_TEST(test_Q008_valid_zero_is_good);
    RUN_TEST(test_negative_values_are_stored_exactly);
    RUN_TEST(test_device_comm_lost_marks_every_parameter_and_keeps_values);
    RUN_TEST(test_UMD005_undeclared_parameter_reads_as_not_supported);
    RUN_TEST(test_UMD005_disabled_device_is_not_supported_and_read_only);
    RUN_TEST(test_UMD009_seq_increments_on_every_write_and_quality_change);
    RUN_TEST(test_publish_rejects_non_finite_values_and_wrong_quality);
    RUN_TEST(test_bad_slot_unknown_parameter_and_null_pointers);
    RUN_TEST(test_calls_before_init_are_refused);
    RUN_TEST(test_init_validates_layout);
    RUN_TEST(test_snapshot_returns_records_in_layout_order);
    RUN_TEST(test_lock_is_taken_balanced_and_never_nested);
}
