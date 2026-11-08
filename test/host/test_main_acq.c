/*
 * MyEMS host unit tests - acquisition core runner (DP-WP4 D6).
 * Uses the generated lab_bench tables in main/generated.
 * Test IDs in names refer to the DP-WP4 Verification & Test Specification v1.1.
 */
#include "unity.h"

void run_acq_core_tests(void);

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    run_acq_core_tests();
    return UNITY_END();
}
