/*
 * Scheduler core tests (DP-WP4 D5): SCH-001..SCH-010 (host part), RTE-001,
 * plan building and the timing budget. Time is simulated in microseconds;
 * a small driver plays the roles of the scheduler task, the master and
 * acquisition (feedback after a configurable delay).
 */
#include <string.h>
#include "unity.h"
#include "ems_sched_core.h"

#define S(x)  ((int64_t)((x) * 1000000.0))
#define MS(x) ((int64_t)(x) * 1000)

/* ------------------------------------------------------------- fixtures --- */
/* Same poll groups as MYEMS_SIM_ACMETER_3PH (Design 8.2). */
static const ems_poll_group_t k_groups[3] = {
    { .interval_ms = 5000u,  .offset_ms = 500u,  .stale_timeout_ms = 11000u, .start_address = 0u,  .count = 14u, .function_code = 3u },
    { .interval_ms = 10000u, .offset_ms = 1500u, .stale_timeout_ms = 21000u, .start_address = 14u, .count = 4u,  .function_code = 3u },
    { .interval_ms = 30000u, .offset_ms = 2500u, .stale_timeout_ms = 61000u, .start_address = 18u, .count = 1u,  .function_code = 3u },
};
static ems_poll_group_t g_groups8[8];
static ems_cfg_map_t g_maps[2];
static const ems_cfg_profile_t k_profiles[2] = {
    { .profile_id = "P", .profile_version = "1", .device_type = "AC", .map_index = 0u },
    { .profile_id = "P8", .profile_version = "1", .device_type = "AC", .map_index = 1u },
};
static const ems_cfg_bus_t k_bus = { .bus_id = "B", .baud = 9600u, .uart_num = 1u, .tx_gpio = 17u,
                                     .rx_gpio = 18u, .rts_gpio = 21u, .parity = EMS_PARITY_EVEN, .stop_bits = 1u };
static ems_cfg_device_t g_dev[5];
static ems_config_build_t g_cfg;

static void make_cfg(uint8_t devices)
{
    memset(g_dev, 0, sizeof(g_dev));
    g_maps[0] = (ems_cfg_map_t){ .map_id = "M", .map_version = "1", .groups = k_groups, .mappings = NULL,
                                 .mapping_count = 0u, .group_count = 3u };
    for (int i = 0; i < 8; i++) {
        g_groups8[i] = k_groups[0];
        g_groups8[i].start_address = (uint16_t)(i * 14);
    }
    g_maps[1] = (ems_cfg_map_t){ .map_id = "M8", .map_version = "1", .groups = g_groups8, .group_count = 8u };
    for (uint8_t i = 0; i < 5; i++) {
        g_dev[i] = (ems_cfg_device_t){ .instance_id = "D", .location = "L", .ct_ratio = 1.0, .pt_ratio = 1.0,
                                       .probe_interval_ms = 30000u, .timeout_ms = 500u, .slot = i,
                                       .profile_index = 0u, .bus_index = 0u, .slave_id = (uint8_t)(i + 1u),
                                       .max_retries = 1u, .comm_lost_threshold = 2u, .probe_group = 2u,
                                       .enabled = true };
    }
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.buses = &k_bus;
    g_cfg.maps = g_maps;
    g_cfg.profiles = k_profiles;
    g_cfg.devices = g_dev;
    g_cfg.bus_count = 1u;
    g_cfg.map_count = 2u;
    g_cfg.profile_count = 2u;
    g_cfg.device_count = devices;
}

/* --------------------------------------------------------------- driver --- */
#define MAX_LOG 4096
typedef struct {
    int64_t  t;
    uint32_t id;
    uint8_t  entry;
    uint8_t  slot;
} sub_t;

typedef struct {
    ems_sched_core_t core;
    sub_t    log[MAX_LOG];
    int      n;
    /* outstanding request per device, for the one-in-flight check and replies */
    uint32_t out_id[EMS_SCHED_MAX_DEVICES];
    int64_t  out_reply_at[EMS_SCHED_MAX_DEVICES];
    int64_t  reply_us[EMS_SCHED_MAX_DEVICES];   /* <0: never reply */
    int      reject_next;                       /* reject this many submissions */
    int      overlap_errors;
    uint32_t guard_bits;
    uint32_t lcg;
    bool     random_reply;
} drv_t;

static drv_t D;

static void drv_init(uint8_t devices, int64_t reply_us)
{
    make_cfg(devices);
    memset(&D, 0, sizeof(D));
    ems_sched_plan_t plan;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_OK, ems_sched_plan_build(&g_cfg, &plan));
    ems_sched_init(&D.core, &plan, 250u, 0);
    for (unsigned i = 0u; i < EMS_SCHED_MAX_DEVICES; i++) {
        D.reply_us[i] = reply_us;
    }
    D.lcg = 12345u;
}

static int64_t next_reply_us(uint8_t slot)
{
    if (!D.random_reply) {
        return D.reply_us[slot];
    }
    D.lcg = D.lcg * 1103515245u + 12345u;
    return MS(50 + (int64_t)((D.lcg >> 16) % 2950u));   /* 50 .. 3000 ms */
}

/* One pass of the scheduler task at time t. */
static void drv_step(int64_t t)
{
    for (uint8_t s = 0; s < EMS_SCHED_MAX_DEVICES; s++) {
        if (D.out_id[s] != 0u && D.out_reply_at[s] >= 0 && t >= D.out_reply_at[s]) {
            (void)ems_sched_feedback(&D.core, D.out_id[s], t);
            D.out_id[s] = 0u;
        }
    }
    D.guard_bits |= ems_sched_check_guards(&D.core, t);
    ems_sched_request_t r;
    while (ems_sched_next(&D.core, t, &r)) {
        if (D.reject_next > 0) {
            D.reject_next--;
            ems_sched_submitted(&D.core, &r, false, t);
            continue;
        }
        if (D.out_id[r.slot] != 0u) {
            D.overlap_errors++;
        }
        ems_sched_submitted(&D.core, &r, true, t);
        D.out_id[r.slot] = r.request_id;
        const int64_t d = next_reply_us(r.slot);
        D.out_reply_at[r.slot] = (d < 0) ? -1 : t + d;
        if (D.n < MAX_LOG) {
            D.log[D.n++] = (sub_t){ .t = t, .id = r.request_id, .entry = r.entry, .slot = r.slot };
        }
    }
}

/* Event-driven run from 'from' to 'until': jump to the next due time,
 * reply time or guard deadline. */
static void drv_run_from(int64_t from, int64_t until)
{
    int64_t t = from;
    for (int guard = 0; guard < 200000; guard++) {
        drv_step(t);
        int64_t nt = ems_sched_next_event_us(&D.core);
        for (uint8_t s = 0; s < EMS_SCHED_MAX_DEVICES; s++) {
            if (D.out_id[s] != 0u && D.out_reply_at[s] >= 0 && D.out_reply_at[s] < nt) {
                nt = D.out_reply_at[s];
            }
        }
        if (nt <= t) {
            nt = t + 1;
        }
        if (nt > until) {
            return;
        }
        t = nt;
    }
    TEST_FAIL_MESSAGE("driver did not finish");
}

static void drv_run(int64_t until)
{
    drv_run_from(0, until);
}

/* Submission times of one entry. */
static int times_of(uint8_t entry, int64_t *out, int max)
{
    int k = 0;
    for (int i = 0; i < D.n && k < max; i++) {
        if (D.log[i].entry == entry) {
            out[k++] = D.log[i].t;
        }
    }
    return k;
}

/* ----------------------------------------------------------------- plan --- */
static void test_plan_entries_and_order(void)
{
    make_cfg(2);
    ems_sched_plan_t p;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_OK, ems_sched_plan_build(&g_cfg, &p));
    TEST_ASSERT_EQUAL_UINT8(6u, p.entry_count);
    for (uint8_t e = 0; e < 6; e++) {
        TEST_ASSERT_EQUAL_UINT8(e / 3u, p.entries[e].slot);
        TEST_ASSERT_EQUAL_UINT8(e % 3u, p.entries[e].group_index);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(e / 3u + 1u), p.entries[e].slave_id);
        TEST_ASSERT_EQUAL(e % 3u == 2u, p.entries[e].is_probe);
    }
    TEST_ASSERT_EQUAL_UINT8(2u, p.devices[0].probe_entry);
    TEST_ASSERT_EQUAL_UINT8(5u, p.devices[1].probe_entry);
    TEST_ASSERT_EQUAL_UINT8(3u, p.devices[1].first_entry);
    TEST_ASSERT_EQUAL_UINT16(14u, p.entries[0].count);
    TEST_ASSERT_EQUAL_UINT32(500u, p.entries[0].timeout_ms);
}

static void test_plan_disabled_device_has_no_entries(void)
{
    make_cfg(2);
    g_dev[0].enabled = false;
    ems_sched_plan_t p;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_OK, ems_sched_plan_build(&g_cfg, &p));
    TEST_ASSERT_EQUAL_UINT8(3u, p.entry_count);
    TEST_ASSERT_EQUAL_UINT8(1u, p.entries[0].slot);
    TEST_ASSERT_EQUAL_UINT8(0u, p.devices[0].entry_count);
    TEST_ASSERT_EQUAL_UINT8(EMS_SCHED_NO_ENTRY, p.devices[0].probe_entry);
}

static void test_plan_rejects_bad_configuration(void)
{
    ems_sched_plan_t p;
    make_cfg(5);
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_TOO_MANY_DEVICES, ems_sched_plan_build(&g_cfg, &p));
    make_cfg(3);
    for (int i = 0; i < 3; i++) {
        g_dev[i].profile_index = 1u;            /* 8 groups each: 24 entries > 16 */
    }
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_TOO_MANY_ENTRIES, ems_sched_plan_build(&g_cfg, &p));
    make_cfg(1);
    g_groups8[0].function_code = 6u;
    g_dev[0].profile_index = 1u;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_BAD_GROUP, ems_sched_plan_build(&g_cfg, &p));
    make_cfg(1);
    g_dev[0].probe_group = 3u;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_BAD_REFERENCE, ems_sched_plan_build(&g_cfg, &p));
    make_cfg(1);
    g_dev[0].bus_index = 1u;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_BAD_REFERENCE, ems_sched_plan_build(&g_cfg, &p));
    make_cfg(1);
    g_dev[0].slave_id = 0u;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_BAD_DEVICE, ems_sched_plan_build(&g_cfg, &p));
    make_cfg(1);
    g_dev[0].max_retries = 6u;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_BAD_DEVICE, ems_sched_plan_build(&g_cfg, &p));
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_NULL, ems_sched_plan_build(NULL, &p));
    TEST_ASSERT_EQUAL_STRING("BAD_DEVICE", ems_sched_plan_status_name(EMS_SCHED_PLAN_BAD_DEVICE));
}

/* Design 9.6 / GEN-006 / GEN-007: same numbers as the host tool's budget. */
static void test_t_wc_matches_design_budget(void)
{
    TEST_ASSERT_EQUAL_INT64(3118334, ems_sched_t_wc_us(9600u, EMS_PARITY_EVEN, 1u, 500u, 1u));
    TEST_ASSERT_EQUAL_INT64(6227501, ems_sched_t_wc_us(9600u, EMS_PARITY_EVEN, 1u, 1000u, 2u));
    TEST_ASSERT_EQUAL_INT64(0, ems_sched_t_wc_us(0u, EMS_PARITY_EVEN, 1u, 500u, 1u));
    make_cfg(1);
    ems_sched_plan_t p;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_OK, ems_sched_plan_build(&g_cfg, &p));
    TEST_ASSERT_EQUAL_INT64(3118334 + 5000000, p.devices[0].guard_us);
}

/* -------------------------------------------------------------- RTE-001 --- */
static void test_RTE001_request_id_encoding(void)
{
    drv_init(2, MS(100));
    ems_sched_request_t r;
    TEST_ASSERT_TRUE(ems_sched_next(&D.core, S(0.5), &r));
    TEST_ASSERT_EQUAL_HEX32(0x00000001u, r.request_id);
    TEST_ASSERT_EQUAL_UINT8(0u, ems_sched_request_entry(r.request_id));
    ems_sched_submitted(&D.core, &r, true, S(0.5));
    TEST_ASSERT_TRUE(ems_sched_next(&D.core, S(0.5), &r));     /* device 1, entry 3 */
    TEST_ASSERT_EQUAL_HEX32(0x03000002u, r.request_id);
    TEST_ASSERT_EQUAL_UINT8(3u, ems_sched_request_entry(r.request_id));
    ems_sched_submitted(&D.core, &r, true, S(0.5));
    (void)ems_sched_feedback(&D.core, 0x00000001u, S(0.6));
    D.core.seq24 = 0xFFFFFEu;
    TEST_ASSERT_TRUE(ems_sched_next(&D.core, S(1.5), &r));     /* energy, entry 1 */
    TEST_ASSERT_EQUAL_HEX32(0x01FFFFFFu, r.request_id);
    ems_sched_submitted(&D.core, &r, true, S(1.5));
    (void)ems_sched_feedback(&D.core, r.request_id, S(1.6));
    TEST_ASSERT_TRUE(ems_sched_next(&D.core, S(2.5), &r));     /* status, entry 2: seq wraps */
    TEST_ASSERT_EQUAL_HEX32(0x02000001u, r.request_id);
    TEST_ASSERT_NOT_EQUAL(0, r.request_id);
}

/* --------------------------------------------------------------- SCH --- */
static void assert_cadence(uint8_t entry, double first_s, double period_s, int expected)
{
    int64_t t[64];
    int n = times_of(entry, t, 64);
    TEST_ASSERT_EQUAL_INT(expected, n);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT64(S(first_s + period_s * i), t[i]);
    }
}

static void test_SCH001_group_intervals_and_offsets(void)
{
    drv_init(1, MS(100));
    drv_run(S(120));
    assert_cadence(0u, 0.5, 5.0, 24);      /* electrical: 0.5, 5.5 ... 115.5 */
    assert_cadence(1u, 1.5, 10.0, 12);     /* energy:     1.5 ... 111.5      */
    assert_cadence(2u, 2.5, 30.0, 4);      /* status:     2.5 ... 92.5       */
    TEST_ASSERT_EQUAL_INT(0, D.overlap_errors);
    TEST_ASSERT_EQUAL_UINT32(0u, D.core.entry[0].stats.skipped);
}

static void test_SCH002_missed_periods_are_skipped_not_caught_up(void)
{
    drv_init(1, MS(100));
    D.reply_us[0] = S(12.0);               /* first request (electrical @0.5) takes 12 s */
    drv_run(S(0.6));
    D.reply_us[0] = MS(100);
    drv_run_from(S(0.6), S(40));
    int64_t t[32];
    int n = times_of(0u, t, 32);
    /* electrical: 0.5, then one poll after the release, then back on the 5 s grid */
    TEST_ASSERT_EQUAL_INT64(S(0.5), t[0]);
    TEST_ASSERT_TRUE(t[1] >= S(12.5) && t[1] < S(15.5));
    TEST_ASSERT_EQUAL_INT64(S(15.5), t[2]);
    TEST_ASSERT_EQUAL_INT64(S(20.5), t[3]);
    for (int i = 2; i + 1 < n; i++) {
        TEST_ASSERT_EQUAL_INT64(S(5.0), t[i + 1] - t[i]);
    }
    /* due at 5.5 and 10.5 while busy: one of them served late, one skipped */
    TEST_ASSERT_EQUAL_UINT32(1u, D.core.entry[0].stats.skipped);
    /* no burst: at most one electrical request between 12.5 and 15.5 */
    int burst = 0;
    for (int i = 0; i < n; i++) {
        burst += (t[i] >= S(12.5) && t[i] < S(15.5)) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT(1, burst);
}

static void test_SCH003_never_two_in_flight_per_device(void)
{
    drv_init(2, 0);
    D.random_reply = true;
    drv_run(S(600));
    TEST_ASSERT_TRUE(D.n > 150);
    TEST_ASSERT_EQUAL_INT(0, D.overlap_errors);
    /* the core's view agrees: a device never has a new submission before feedback */
    for (int i = 1; i < D.n; i++) {
        for (int j = i - 1; j >= 0; j--) {
            if (D.log[j].slot == D.log[i].slot) {
                TEST_ASSERT_TRUE(D.log[i].t > D.log[j].t);
                break;
            }
        }
    }
}

static void test_SCH004_two_devices_due_together(void)
{
    drv_init(2, MS(100));
    drv_run(S(0.5));
    TEST_ASSERT_EQUAL_INT(2, D.n);                     /* both served at 0.5 */
    TEST_ASSERT_EQUAL_UINT8(0u, D.log[0].entry);       /* tie: lower entry first */
    TEST_ASSERT_EQUAL_UINT8(3u, D.log[1].entry);
    TEST_ASSERT_EQUAL_INT64(S(0.5), D.log[1].t);
}

static void test_SCH004_earliest_deadline_first(void)
{
    drv_init(1, MS(100));
    D.reply_us[0] = S(5.6);                /* electrical @0.5 busy until 6.1 */
    drv_run(S(0.6));
    D.reply_us[0] = MS(100);
    drv_run_from(S(0.6), S(6.5));
    /* after release at 6.1: energy (due 1.5), status (2.5), electrical (5.5) */
    TEST_ASSERT_EQUAL_INT(4, D.n);
    TEST_ASSERT_EQUAL_UINT8(1u, D.log[1].entry);
    TEST_ASSERT_EQUAL_UINT8(2u, D.log[2].entry);
    TEST_ASSERT_EQUAL_UINT8(0u, D.log[3].entry);
    TEST_ASSERT_EQUAL_INT64(S(6.1), D.log[1].t);
    TEST_ASSERT_EQUAL_INT64(S(6.2), D.log[2].t);
    TEST_ASSERT_EQUAL_INT64(S(6.3), D.log[3].t);
}

static void test_SCH005_rejected_submission_retried_after_250ms(void)
{
    drv_init(1, MS(100));
    D.reject_next = 2;
    drv_run(S(1.4));
    TEST_ASSERT_EQUAL_INT(1, D.n);
    TEST_ASSERT_EQUAL_INT64(S(1.0), D.log[0].t);       /* 0.5 rejected, 0.75 rejected, 1.0 ok */
    TEST_ASSERT_EQUAL_UINT32(2u, D.core.entry[0].stats.rejected);
    TEST_ASSERT_EQUAL_UINT32(1u, D.core.entry[0].stats.submitted);
}

static void test_SCH006_offline_device_only_probed(void)
{
    drv_init(2, MS(100));
    drv_run(S(10));
    TEST_ASSERT_TRUE(ems_sched_set_device_mode(&D.core, 0u, EMS_SCHED_DEV_OFFLINE, S(10)));
    const int before = D.n;
    drv_run_from(S(10), S(310));
    int probes = 0;
    int64_t last = 0;
    for (int i = before; i < D.n; i++) {
        if (D.log[i].slot != 0u) {
            continue;
        }
        TEST_ASSERT_EQUAL_UINT8(2u, D.log[i].entry);   /* only the status group */
        if (probes == 0) {
            TEST_ASSERT_EQUAL_INT64(S(40), D.log[i].t);
        } else {
            TEST_ASSERT_EQUAL_INT64(S(30), D.log[i].t - last);
        }
        last = D.log[i].t;
        probes++;
    }
    TEST_ASSERT_EQUAL_INT(10, probes);                 /* 40, 70, ... 310 */
    /* device 1 is unaffected */
    int dev1 = 0;
    for (int i = before; i < D.n; i++) {
        dev1 += (D.log[i].slot == 1u && D.log[i].entry == 3u) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT(60, dev1);
}

/* The probe cadence is the device's probe interval, not the group interval. */
static void test_SCH006_probe_interval_differs_from_group_interval(void)
{
    make_cfg(1);
    g_dev[0].probe_interval_ms = 60000u;          /* status group interval is 30 s */
    memset(&D, 0, sizeof(D));
    ems_sched_plan_t p;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_OK, ems_sched_plan_build(&g_cfg, &p));
    ems_sched_init(&D.core, &p, 250u, 0);
    D.reply_us[0] = MS(100);
    TEST_ASSERT_TRUE(ems_sched_set_device_mode(&D.core, 0u, EMS_SCHED_DEV_OFFLINE, 0));
    drv_run(S(200));
    int64_t t[16];
    TEST_ASSERT_EQUAL_INT(3, times_of(2u, t, 16));    /* 60, 120, 180 */
    TEST_ASSERT_EQUAL_INT64(S(60), t[0]);
    TEST_ASSERT_EQUAL_INT64(S(120), t[1]);
    TEST_ASSERT_EQUAL_INT64(S(180), t[2]);
    TEST_ASSERT_EQUAL_INT(3, D.n);                     /* nothing else polled */
}

static void test_SCH007_back_online_all_groups_immediately(void)
{
    drv_init(1, MS(100));
    drv_run(S(3));
    (void)ems_sched_set_device_mode(&D.core, 0u, EMS_SCHED_DEV_OFFLINE, S(3));
    drv_run_from(S(3), S(50));
    const int before = D.n;
    TEST_ASSERT_TRUE(ems_sched_set_device_mode(&D.core, 0u, EMS_SCHED_DEV_ONLINE, S(50)));
    drv_run_from(S(50), S(50.35));
    TEST_ASSERT_EQUAL_INT(before + 3, D.n);
    TEST_ASSERT_EQUAL_UINT8(0u, D.log[before].entry);
    TEST_ASSERT_EQUAL_INT64(S(50.0), D.log[before].t);
    TEST_ASSERT_EQUAL_UINT8(1u, D.log[before + 1].entry);
    TEST_ASSERT_EQUAL_INT64(S(50.1), D.log[before + 1].t);
    TEST_ASSERT_EQUAL_UINT8(2u, D.log[before + 2].entry);
    TEST_ASSERT_EQUAL_INT64(S(50.2), D.log[before + 2].t);
    /* then normal cadence from the new grid */
    drv_run_from(S(50.35), S(56));
    TEST_ASSERT_EQUAL_INT64(S(55.0), D.log[D.n - 1].t);
    TEST_ASSERT_EQUAL_UINT8(0u, D.log[D.n - 1].entry);
}

static void test_SCH008_guard_expiry_does_not_release_slot(void)
{
    drv_init(2, MS(100));
    D.reply_us[0] = -1;                    /* device 0 feedback lost */
    drv_run(S(120));
    /* exactly one request ever for device 0 */
    int dev0 = 0;
    for (int i = 0; i < D.n; i++) {
        dev0 += (D.log[i].slot == 0u) ? 1 : 0;
    }
    TEST_ASSERT_EQUAL_INT(1, dev0);
    TEST_ASSERT_EQUAL_UINT32(1u, D.core.stats.guard_expired);      /* counted once */
    TEST_ASSERT_EQUAL_UINT32(0x1u, D.guard_bits);
    TEST_ASSERT_EQUAL_HEX32(EMS_SCHED_HEALTH_FEEDBACK, D.core.health_flags);
    TEST_ASSERT_TRUE(D.core.device[0].guard_expired);
    TEST_ASSERT_NOT_EQUAL(0, D.core.device[0].pending_id);         /* still pending */
    TEST_ASSERT_EQUAL_INT(0, D.overlap_errors);
    /* device 1 keeps its cadence */
    int64_t t[32];
    TEST_ASSERT_EQUAL_INT(24, times_of(3u, t, 32));
}

static void test_SCH008_guard_expires_at_t_wc_plus_5s(void)
{
    drv_init(1, MS(100));
    ems_sched_request_t r;
    TEST_ASSERT_TRUE(ems_sched_next(&D.core, S(0.5), &r));
    ems_sched_submitted(&D.core, &r, true, S(0.5));
    const int64_t deadline = S(0.5) + 3118334 + 5000000;
    TEST_ASSERT_EQUAL_INT64(deadline, ems_sched_next_event_us(&D.core));
    TEST_ASSERT_EQUAL_HEX32(0u, ems_sched_check_guards(&D.core, deadline - 1));
    TEST_ASSERT_EQUAL_HEX32(1u, ems_sched_check_guards(&D.core, deadline));
    TEST_ASSERT_EQUAL_HEX32(0u, ems_sched_check_guards(&D.core, deadline + S(60)));
    TEST_ASSERT_FALSE(ems_sched_next(&D.core, deadline + S(60), &r));
}

static void test_SCH009_late_feedback_releases_and_clears_flag(void)
{
    drv_init(1, -1);
    drv_run(S(20));
    TEST_ASSERT_EQUAL_INT(1, D.n);
    TEST_ASSERT_EQUAL_HEX32(EMS_SCHED_HEALTH_FEEDBACK, D.core.health_flags);
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_FB_LATE, ems_sched_feedback(&D.core, D.log[0].id, S(20)));
    D.out_id[0] = 0u;
    TEST_ASSERT_EQUAL_HEX32(0u, D.core.health_flags);
    TEST_ASSERT_EQUAL_UINT32(1u, D.core.stats.feedback_late);
    D.reply_us[0] = MS(100);
    drv_run_from(S(20), S(31));
    TEST_ASSERT_TRUE(D.n >= 4);                        /* polling resumed */
    TEST_ASSERT_EQUAL_INT64(S(20), D.log[1].t);
}

static void test_SCH009_flag_stays_while_another_device_is_stuck(void)
{
    drv_init(2, -1);
    drv_run(S(20));
    TEST_ASSERT_EQUAL_UINT32(2u, D.core.stats.guard_expired);
    (void)ems_sched_feedback(&D.core, D.core.device[0].pending_id, S(20));
    TEST_ASSERT_EQUAL_HEX32(EMS_SCHED_HEALTH_FEEDBACK, D.core.health_flags);
    (void)ems_sched_feedback(&D.core, D.core.device[1].pending_id, S(20));
    TEST_ASSERT_EQUAL_HEX32(0u, D.core.health_flags);
}

static void test_SCH010_non_matching_feedback_ignored(void)
{
    drv_init(1, -1);
    drv_run(S(1));
    const uint32_t id = D.core.device[0].pending_id;
    TEST_ASSERT_NOT_EQUAL(0, id);
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_FB_UNMATCHED, ems_sched_feedback(&D.core, id + 1u, S(1)));
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_FB_UNMATCHED, ems_sched_feedback(&D.core, id ^ 0x01000000u, S(1)));
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_FB_UNMATCHED, ems_sched_feedback(&D.core, 0u, S(1)));
    TEST_ASSERT_EQUAL_UINT32(id, D.core.device[0].pending_id);   /* still pending */
    TEST_ASSERT_EQUAL_UINT32(3u, D.core.stats.feedback_unmatched);
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_FB_MATCHED, ems_sched_feedback(&D.core, id, S(1)));
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_FB_UNMATCHED, ems_sched_feedback(&D.core, id, S(1)));  /* twice */
}

/* --------------------------------------------------------- mode API --- */
static void test_device_mode_api(void)
{
    make_cfg(2);
    g_dev[1].enabled = false;
    ems_sched_plan_t p;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_OK, ems_sched_plan_build(&g_cfg, &p));
    ems_sched_core_t c;
    ems_sched_init(&c, &p, 0u, 0);
    TEST_ASSERT_EQUAL_UINT32(250u, c.submit_retry_ms);
    TEST_ASSERT_EQUAL_UINT8(EMS_SCHED_DEV_DISABLED, c.device[1].mode);
    TEST_ASSERT_FALSE(ems_sched_set_device_mode(&c, 1u, EMS_SCHED_DEV_ONLINE, 0));   /* disabled */
    TEST_ASSERT_FALSE(ems_sched_set_device_mode(&c, 2u, EMS_SCHED_DEV_ONLINE, 0));   /* no slot */
    TEST_ASSERT_FALSE(ems_sched_set_device_mode(&c, 0u, EMS_SCHED_DEV_DISABLED, 0));
    TEST_ASSERT_TRUE(ems_sched_set_device_mode(&c, 0u, EMS_SCHED_DEV_ONLINE, 0));     /* no change */
    TEST_ASSERT_EQUAL_UINT32(0u, c.stats.mode_changes);
    TEST_ASSERT_TRUE(ems_sched_set_device_mode(&c, 0u, EMS_SCHED_DEV_OFFLINE, 0));
    TEST_ASSERT_TRUE(ems_sched_set_device_mode(&c, 0u, EMS_SCHED_DEV_OFFLINE, 0));
    TEST_ASSERT_EQUAL_UINT32(1u, c.stats.mode_changes);
}

static void test_nothing_to_do_when_all_disabled(void)
{
    make_cfg(1);
    g_dev[0].enabled = false;
    ems_sched_plan_t p;
    TEST_ASSERT_EQUAL_INT(EMS_SCHED_PLAN_OK, ems_sched_plan_build(&g_cfg, &p));
    ems_sched_core_t c;
    ems_sched_init(&c, &p, 250u, 0);
    ems_sched_request_t r;
    TEST_ASSERT_FALSE(ems_sched_next(&c, S(1000), &r));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, ems_sched_next_event_us(&c));
    TEST_ASSERT_FALSE(ems_sched_next(NULL, 0, &r));
}

static void test_stale_submission_report_is_ignored(void)
{
    drv_init(1, MS(100));
    ems_sched_request_t r;
    TEST_ASSERT_TRUE(ems_sched_next(&D.core, S(0.5), &r));
    ems_sched_request_t other = r;
    other.request_id ^= 1u;
    ems_sched_submitted(&D.core, &other, false, S(0.5));            /* not the pending one */
    TEST_ASSERT_EQUAL_UINT32(r.request_id, D.core.device[0].pending_id);
    TEST_ASSERT_EQUAL_UINT32(0u, D.core.entry[0].stats.rejected);
}

/* Design 9.9: per-group latency, submit -> feedback. */
static void test_latency_per_entry(void)
{
    drv_init(1, MS(100));
    ems_sched_request_t r;
    TEST_ASSERT_TRUE(ems_sched_next(&D.core, S(0.5), &r));
    ems_sched_submitted(&D.core, &r, true, S(0.5));
    (void)ems_sched_feedback(&D.core, r.request_id, S(0.5) + MS(140));
    TEST_ASSERT_EQUAL_UINT32(140u, D.core.entry[0].stats.last_latency_ms);
    TEST_ASSERT_EQUAL_UINT32(140u, D.core.entry[0].stats.max_latency_ms);
    /* serve energy (1.5) and status (2.5) so that electrical is next at 5.5 */
    for (int k = 0; k < 2; k++) {
        TEST_ASSERT_TRUE(ems_sched_next(&D.core, S(3), &r));
        ems_sched_submitted(&D.core, &r, true, S(3));
        (void)ems_sched_feedback(&D.core, r.request_id, S(3) + MS(25));
    }
    TEST_ASSERT_EQUAL_UINT32(25u, D.core.entry[1].stats.last_latency_ms);
    TEST_ASSERT_TRUE(ems_sched_next(&D.core, S(5.5), &r));
    TEST_ASSERT_EQUAL_UINT8(0u, r.entry);
    ems_sched_submitted(&D.core, &r, true, S(5.5));
    (void)ems_sched_feedback(&D.core, r.request_id, S(5.5) + MS(60));
    TEST_ASSERT_EQUAL_UINT32(60u, D.core.entry[0].stats.last_latency_ms);
    TEST_ASSERT_EQUAL_UINT32(140u, D.core.entry[0].stats.max_latency_ms);
    (void)ems_sched_feedback(&D.core, 0x12345678u, S(9));     /* unmatched: no change */
    TEST_ASSERT_EQUAL_UINT32(60u, D.core.entry[0].stats.last_latency_ms);
}

void run_sched_core_tests(void)
{
    RUN_TEST(test_plan_entries_and_order);
    RUN_TEST(test_plan_disabled_device_has_no_entries);
    RUN_TEST(test_plan_rejects_bad_configuration);
    RUN_TEST(test_t_wc_matches_design_budget);
    RUN_TEST(test_RTE001_request_id_encoding);
    RUN_TEST(test_SCH001_group_intervals_and_offsets);
    RUN_TEST(test_SCH002_missed_periods_are_skipped_not_caught_up);
    RUN_TEST(test_SCH003_never_two_in_flight_per_device);
    RUN_TEST(test_SCH004_two_devices_due_together);
    RUN_TEST(test_SCH004_earliest_deadline_first);
    RUN_TEST(test_SCH005_rejected_submission_retried_after_250ms);
    RUN_TEST(test_SCH006_offline_device_only_probed);
    RUN_TEST(test_SCH006_probe_interval_differs_from_group_interval);
    RUN_TEST(test_SCH007_back_online_all_groups_immediately);
    RUN_TEST(test_SCH008_guard_expiry_does_not_release_slot);
    RUN_TEST(test_SCH008_guard_expires_at_t_wc_plus_5s);
    RUN_TEST(test_SCH009_late_feedback_releases_and_clears_flag);
    RUN_TEST(test_SCH009_flag_stays_while_another_device_is_stuck);
    RUN_TEST(test_SCH010_non_matching_feedback_ignored);
    RUN_TEST(test_device_mode_api);
    RUN_TEST(test_nothing_to_do_when_all_disabled);
    RUN_TEST(test_stale_submission_report_is_ignored);
    RUN_TEST(test_latency_per_entry);
}
