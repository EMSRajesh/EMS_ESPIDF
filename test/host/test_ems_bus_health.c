/*
 * Bus health tests (DP-WP4 D8, Design 9.7): host part of ESC-001..ESC-005.
 * Time is simulated in microseconds; inputs are what the acquisition task
 * reads from rs485_recovery_get_state() and modbus_master_get_stats().
 */
#include <string.h>
#include "unity.h"
#include "ems_bus_health.h"

#define S(x)  ((int64_t)((x) * 1000000.0))
#define MS(x) ((int64_t)(x) * 1000)

static ems_bus_health_t H;

static void start(uint32_t stall_ms)
{
    const ems_bus_health_config_t cfg = { .recovery_limit_ms = 0u, .stall_limit_ms = stall_ms };
    ems_bus_health_init(&H, &cfg, 0);
}

static ems_bus_inputs_t in_tr(ems_bus_transport_t tr)
{
    return (ems_bus_inputs_t){ .transport = (uint8_t)tr, .master_report = EMS_BUS_REPORT_NONE,
                               .counters_valid = true, .accepted = 10u, .completed = 10u };
}

static ems_bus_change_t upd(ems_bus_inputs_t in, int64_t t)
{
    return ems_bus_health_update(&H, &in, t);
}

/* A normal recovery after a failed transaction is RECOVERING, never FAULT. */
static void test_normal_recovery_is_not_a_fault(void)
{
    start(8118u);
    TEST_ASSERT_EQUAL_INT(EMS_BUS_OK, ems_bus_health_state(&H));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in_tr(EMS_BUS_TR_RECOVERING), S(1)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_RECOVERING, ems_bus_health_state(&H));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in_tr(EMS_BUS_TR_RECOVERING), S(2.1)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in_tr(EMS_BUS_TR_IDLE), S(2.2)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_OK, ems_bus_health_state(&H));
    TEST_ASSERT_EQUAL_UINT32(0u, H.faults);
}

/* ESC-002: transport recovery FAILED -> BUS_FAULT once, reason TRANSPORT_FAILED. */
static void test_ESC002_transport_failed_is_fault_once(void)
{
    start(8118u);
    (void)upd(in_tr(EMS_BUS_TR_RECOVERING), S(1));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_FAULT, upd(in_tr(EMS_BUS_TR_FAILED), S(3)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_FAULT, ems_bus_health_state(&H));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_REASON_TRANSPORT_FAILED, H.reason);
    TEST_ASSERT_EQUAL_INT64(S(3), H.fault_since_us);
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in_tr(EMS_BUS_TR_FAILED), S(3.1)));
    TEST_ASSERT_EQUAL_UINT32(1u, H.faults);
}

/* Without a master health report the fault is latched (fail closed): the
 * 09_057 master never leaves its bus-unavailable state, even if the transport
 * looks idle again. */
static void test_ESC005_fault_latched_without_master_report(void)
{
    start(8118u);
    (void)upd(in_tr(EMS_BUS_TR_FAILED), S(3));
    for (int k = 0; k < 100; k++) {
        TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in_tr(EMS_BUS_TR_IDLE), S(4) + S(k)));
    }
    TEST_ASSERT_EQUAL_INT(EMS_BUS_FAULT, ems_bus_health_state(&H));
    TEST_ASSERT_EQUAL_UINT32(0u, H.recoveries);
}

/* Master gave up on its own deadline while the transport later completed:
 * the transport stays latched COMPLETE (never released) -> RECOVERY_STUCK. */
static void test_recovery_stuck_after_limit(void)
{
    start(0u);
    (void)upd(in_tr(EMS_BUS_TR_RECOVERING), S(10));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in_tr(EMS_BUS_TR_RECOVERING), S(14.999)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_FAULT, upd(in_tr(EMS_BUS_TR_RECOVERING), S(15)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_REASON_RECOVERY_STUCK, H.reason);
}

static void test_recovery_limit_from_config(void)
{
    const ems_bus_health_config_t cfg = { .recovery_limit_ms = 2000u, .stall_limit_ms = 0u };
    ems_bus_health_init(&H, &cfg, 0);
    TEST_ASSERT_EQUAL_UINT32(2000u, H.cfg.recovery_limit_ms);
    (void)upd(in_tr(EMS_BUS_TR_RECOVERING), S(1));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in_tr(EMS_BUS_TR_RECOVERING), S(2.999)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_FAULT, upd(in_tr(EMS_BUS_TR_RECOVERING), S(3)));
}

/* Back-to-back normal recoveries restart the timer; UNKNOWN does not. */
static void test_recovery_timer_restarts_on_idle_only(void)
{
    start(0u);
    (void)upd(in_tr(EMS_BUS_TR_RECOVERING), S(0));
    (void)upd(in_tr(EMS_BUS_TR_IDLE), S(4));
    (void)upd(in_tr(EMS_BUS_TR_RECOVERING), S(4.5));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in_tr(EMS_BUS_TR_RECOVERING), S(9)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in_tr(EMS_BUS_TR_UNKNOWN), S(9.4)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_FAULT, upd(in_tr(EMS_BUS_TR_UNKNOWN), S(9.5)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_REASON_RECOVERY_STUCK, H.reason);
}

/* ESC-001 situation: the master holds a request and completes nothing. */
static void test_master_stalled_after_guard(void)
{
    start(8118u);
    ems_bus_inputs_t in = in_tr(EMS_BUS_TR_IDLE);
    (void)upd(in, S(0));                       /* nothing outstanding */
    in.accepted = 11u;                         /* request submitted, never completed */
    (void)upd(in, S(1));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(8.117)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_FAULT, upd(in, S(8.118)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_REASON_MASTER_STALLED, H.reason);
    TEST_ASSERT_EQUAL_UINT32(1u, H.outstanding);
}

/* Completions are progress; a busy but working master never stalls. */
static void test_progress_resets_stall_timer(void)
{
    start(8118u);
    ems_bus_inputs_t in = in_tr(EMS_BUS_TR_IDLE);
    in.accepted = 12u;                         /* always 2 outstanding */
    for (int k = 0; k <= 100; k++) {
        in.accepted++;
        in.completed++;
        TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(3.1) * k));
    }
    TEST_ASSERT_EQUAL_INT(EMS_BUS_OK, ems_bus_health_state(&H));
    /* idle master (nothing outstanding) never stalls either */
    in.accepted = in.completed;
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(1000)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(5000)));
}

/* A long idle period (nothing outstanding, nothing completed) followed by a
 * new request is not a stall: the stall time starts with the request. */
static void test_idle_then_new_request_is_not_a_stall(void)
{
    start(8118u);
    ems_bus_inputs_t in = in_tr(EMS_BUS_TR_IDLE);
    for (int k = 0; k <= 60; k++) {
        TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(k)));
    }
    in.accepted++;                                 /* probe after 60 s of nothing */
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(60.1)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(63.0)));
    in.completed++;
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(63.2)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_OK, ems_bus_health_state(&H));
}

/* Unreadable counters or stall limit 0: no stall check. Counters wrap. */
static void test_stall_check_disabled_and_counter_wrap(void)
{
    start(8118u);
    ems_bus_inputs_t in = in_tr(EMS_BUS_TR_IDLE);
    in.accepted = 11u;
    in.counters_valid = false;
    (void)upd(in, S(0));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(100)));
    start(0u);
    in.counters_valid = true;
    (void)upd(in, S(0));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(100)));
    start(8118u);
    in.accepted = 2u;
    in.completed = 0xFFFFFFFFu;
    (void)upd(in, S(0));
    TEST_ASSERT_EQUAL_UINT32(3u, H.outstanding);
}

/* After CR-D8-01: the master reports its health; FAULT is left on OK. */
static void test_ESC005_master_report_drives_recovery(void)
{
    start(8118u);
    ems_bus_inputs_t in = in_tr(EMS_BUS_TR_IDLE);
    in.master_report = EMS_BUS_REPORT_RECOVERING;
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(1)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_RECOVERING, ems_bus_health_state(&H));
    in.master_report = EMS_BUS_REPORT_FAULT;
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_FAULT, upd(in, S(2)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_REASON_MASTER_REPORT, H.reason);
    in.master_report = EMS_BUS_REPORT_RECOVERING;           /* backoff attempt running */
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(4)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_FAULT, ems_bus_health_state(&H));   /* not proven OK yet */
    TEST_ASSERT_EQUAL_UINT32(0u, H.recoveries);
    in.master_report = EMS_BUS_REPORT_OK;
    in.transport = EMS_BUS_TR_RECOVERING;                   /* normal recovery right after: fine */
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_OK, upd(in, S(7)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_RECOVERING, ems_bus_health_state(&H));
    TEST_ASSERT_EQUAL_UINT32(1u, H.recoveries);
}

static void test_master_report_ok_after_fault_is_change_ok(void)
{
    start(8118u);
    ems_bus_inputs_t in = in_tr(EMS_BUS_TR_IDLE);
    in.master_report = EMS_BUS_REPORT_FAULT;
    (void)upd(in, S(1));
    in.master_report = EMS_BUS_REPORT_OK;
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_OK, upd(in, S(31)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_OK, ems_bus_health_state(&H));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(32)));
    TEST_ASSERT_EQUAL_UINT32(1u, H.faults);
    TEST_ASSERT_EQUAL_UINT32(1u, H.recoveries);
}

/* A derived fault wins over a master that says OK (fail closed). */
static void test_derived_fault_overrides_master_ok(void)
{
    start(8118u);
    ems_bus_inputs_t in = in_tr(EMS_BUS_TR_FAILED);
    in.master_report = EMS_BUS_REPORT_OK;
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_FAULT, upd(in, S(1)));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_REASON_TRANSPORT_FAILED, H.reason);
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, upd(in, S(2)));
    in.transport = EMS_BUS_TR_IDLE;
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_OK, upd(in, S(3)));
}

static void test_names_defaults_and_null(void)
{
    TEST_ASSERT_EQUAL_STRING("BUS_OK", ems_bus_state_name(EMS_BUS_OK));
    TEST_ASSERT_EQUAL_STRING("BUS_RECOVERING", ems_bus_state_name(EMS_BUS_RECOVERING));
    TEST_ASSERT_EQUAL_STRING("BUS_FAULT", ems_bus_state_name(EMS_BUS_FAULT));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", ems_bus_state_name((ems_bus_state_t)9));
    TEST_ASSERT_EQUAL_STRING("NONE", ems_bus_reason_name(EMS_BUS_REASON_NONE));
    TEST_ASSERT_EQUAL_STRING("TRANSPORT_FAILED", ems_bus_reason_name(EMS_BUS_REASON_TRANSPORT_FAILED));
    TEST_ASSERT_EQUAL_STRING("RECOVERY_STUCK", ems_bus_reason_name(EMS_BUS_REASON_RECOVERY_STUCK));
    TEST_ASSERT_EQUAL_STRING("MASTER_STALLED", ems_bus_reason_name(EMS_BUS_REASON_MASTER_STALLED));
    TEST_ASSERT_EQUAL_STRING("MASTER_REPORT", ems_bus_reason_name(EMS_BUS_REASON_MASTER_REPORT));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", ems_bus_reason_name((ems_bus_reason_t)9));
    TEST_ASSERT_EQUAL_STRING("IDLE", ems_bus_transport_name(EMS_BUS_TR_IDLE));
    TEST_ASSERT_EQUAL_STRING("RECOVERING", ems_bus_transport_name(EMS_BUS_TR_RECOVERING));
    TEST_ASSERT_EQUAL_STRING("FAILED", ems_bus_transport_name(EMS_BUS_TR_FAILED));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", ems_bus_transport_name(EMS_BUS_TR_UNKNOWN));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", ems_bus_transport_name((ems_bus_transport_t)9));
    start(0u);
    TEST_ASSERT_EQUAL_UINT32(EMS_BUS_RECOVERY_LIMIT_MS, H.cfg.recovery_limit_ms);
    ems_bus_health_init(&H, NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(EMS_BUS_RECOVERY_LIMIT_MS, H.cfg.recovery_limit_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, H.cfg.stall_limit_ms);
    ems_bus_inputs_t in = in_tr(EMS_BUS_TR_IDLE);
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, ems_bus_health_update(NULL, &in, 0));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_CHANGE_NONE, ems_bus_health_update(&H, NULL, 0));
    TEST_ASSERT_EQUAL_INT(EMS_BUS_FAULT, ems_bus_health_state(NULL));
    ems_bus_health_init(NULL, NULL, 0);
}

void run_bus_health_tests(void)
{
    RUN_TEST(test_normal_recovery_is_not_a_fault);
    RUN_TEST(test_ESC002_transport_failed_is_fault_once);
    RUN_TEST(test_ESC005_fault_latched_without_master_report);
    RUN_TEST(test_recovery_stuck_after_limit);
    RUN_TEST(test_recovery_limit_from_config);
    RUN_TEST(test_recovery_timer_restarts_on_idle_only);
    RUN_TEST(test_master_stalled_after_guard);
    RUN_TEST(test_progress_resets_stall_timer);
    RUN_TEST(test_idle_then_new_request_is_not_a_stall);
    RUN_TEST(test_stall_check_disabled_and_counter_wrap);
    RUN_TEST(test_ESC005_master_report_drives_recovery);
    RUN_TEST(test_master_report_ok_after_fault_is_change_ok);
    RUN_TEST(test_derived_fault_overrides_master_ok);
    RUN_TEST(test_names_defaults_and_null);
}
