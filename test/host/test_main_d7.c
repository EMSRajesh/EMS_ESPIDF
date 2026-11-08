/*
 * MyEMS host unit tests - activation checks and event ring (DP-WP4 D7).
 * Uses the generated lab_bench tables in main/generated.
 * Test IDs in names refer to the DP-WP4 Verification & Test Specification v1.1.
 */
#include "unity.h"

void run_profile_activate_tests(void);
void run_event_tests(void);

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    run_profile_activate_tests();
    run_event_tests();
    return UNITY_END();
}
