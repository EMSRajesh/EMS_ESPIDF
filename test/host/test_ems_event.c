/*
 * Event ring tests (DP-WP4 D7): DIAG-003 overflow, sequence numbers, read-back.
 */
#include "unity.h"
#include "ems_event.h"

static int s_locks;
static int s_unlocks;
static void tl(void *ctx) { (void)ctx; s_locks++; }
static void tu(void *ctx) { (void)ctx; s_unlocks++; }

static void test_emit_and_read_back(void)
{
    ems_event_init(NULL);
    TEST_ASSERT_EQUAL_UINT32(0u, ems_event_oldest_seq());
    TEST_ASSERT_EQUAL_UINT32(1u, ems_event_emit(EMS_EVT_DEVICE_OFFLINE, 0u, EMS_EVENT_NO_PARAM, 2u, 1000));
    TEST_ASSERT_EQUAL_UINT32(2u, ems_event_emit(EMS_EVT_MODBUS_EXCEPTION, 1u, 0x0140u, 0x02u, 2000));
    ems_event_t e;
    TEST_ASSERT_TRUE(ems_event_get(2u, &e));
    TEST_ASSERT_EQUAL_UINT8(EMS_EVT_MODBUS_EXCEPTION, e.type);
    TEST_ASSERT_EQUAL_UINT8(1u, e.slot);
    TEST_ASSERT_EQUAL_UINT16(0x0140u, e.param_id);
    TEST_ASSERT_EQUAL_UINT32(2u, e.data);
    TEST_ASSERT_EQUAL_INT64(2000, e.mono_us);
    TEST_ASSERT_FALSE(ems_event_get(0u, &e));
    TEST_ASSERT_FALSE(ems_event_get(3u, &e));
    TEST_ASSERT_FALSE(ems_event_get(1u, NULL));
    TEST_ASSERT_EQUAL_UINT32(1u, ems_event_oldest_seq());
}

static void test_invalid_type_rejected(void)
{
    ems_event_init(NULL);
    TEST_ASSERT_EQUAL_UINT32(0u, ems_event_emit(EMS_EVT_NONE, 0u, 0u, 0u, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, ems_event_emit(EMS_EVT_TYPE_COUNT, 0u, 0u, 0u, 0));
    ems_event_stats_t st;
    ems_event_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(0u, st.emitted);
}

/* DIAG-003: burst of 100 events into a 64-entry ring. */
static void test_DIAG003_overflow_overwrites_oldest_and_counts_drops(void)
{
    ems_event_init(NULL);
    for (uint32_t i = 1u; i <= 100u; i++) {
        TEST_ASSERT_EQUAL_UINT32(i, ems_event_emit(EMS_EVT_COUNTER_DISCONTINUITY, 0u, 0x0140u, i, (int64_t)i));
    }
    ems_event_stats_t st;
    ems_event_get_stats(&st);
    TEST_ASSERT_EQUAL_UINT32(100u, st.emitted);
    TEST_ASSERT_EQUAL_UINT32(EMS_EVENT_RING_SIZE, st.stored);
    TEST_ASSERT_EQUAL_UINT32(100u - EMS_EVENT_RING_SIZE, st.drops);
    TEST_ASSERT_EQUAL_UINT32(100u - EMS_EVENT_RING_SIZE + 1u, ems_event_oldest_seq());
    ems_event_t e;
    TEST_ASSERT_FALSE(ems_event_get(36u, &e));             /* overwritten */
    TEST_ASSERT_TRUE(ems_event_get(37u, &e));              /* oldest kept */
    TEST_ASSERT_EQUAL_UINT32(37u, e.data);
    TEST_ASSERT_TRUE(ems_event_get(100u, &e));
    TEST_ASSERT_EQUAL_UINT32(100u, e.data);
}

static void test_lock_is_used_and_balanced(void)
{
    const ems_event_lock_t lk = { .lock = tl, .unlock = tu, .ctx = NULL };
    s_locks = s_unlocks = 0;
    ems_event_init(&lk);
    (void)ems_event_emit(EMS_EVT_BUS_OK, EMS_EVENT_NO_SLOT, 0u, 0u, 1);
    ems_event_t e;
    (void)ems_event_get(1u, &e);
    ems_event_stats_t st;
    ems_event_get_stats(&st);
    (void)ems_event_oldest_seq();
    TEST_ASSERT_EQUAL_INT(4, s_locks);
    TEST_ASSERT_EQUAL_INT(s_locks, s_unlocks);
    ems_event_init(NULL);
}

static void test_type_names(void)
{
    TEST_ASSERT_EQUAL_STRING("DEVICE_OFFLINE", ems_event_type_name(EMS_EVT_DEVICE_OFFLINE));
    TEST_ASSERT_EQUAL_STRING("CONFIG_REJECTED", ems_event_type_name(EMS_EVT_CONFIG_REJECTED));
    TEST_ASSERT_EQUAL_STRING("SCHED_GUARD_EXPIRED", ems_event_type_name(EMS_EVT_SCHED_GUARD_EXPIRED));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", ems_event_type_name(EMS_EVT_NONE));
}

void run_event_tests(void)
{
    RUN_TEST(test_emit_and_read_back);
    RUN_TEST(test_invalid_type_rejected);
    RUN_TEST(test_DIAG003_overflow_overwrites_oldest_and_counts_drops);
    RUN_TEST(test_lock_is_used_and_balanced);
    RUN_TEST(test_type_names);
}
