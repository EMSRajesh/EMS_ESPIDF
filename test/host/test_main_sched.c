/*
 * MyEMS host unit tests - poll scheduler core runner (DP-WP4 D5).
 * Test IDs in names refer to the DP-WP4 Verification & Test Specification v1.1.
 */
#include "unity.h"

void run_sched_core_tests(void);

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    run_sched_core_tests();
    return UNITY_END();
}
