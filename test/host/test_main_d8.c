/*
 * MyEMS host unit tests - bus health (DP-WP4 D8, Design 9.7).
 * Test IDs in names refer to the DP-WP4 Verification & Test Specification v1.1.
 */
#include "unity.h"

void run_bus_health_tests(void);

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    run_bus_health_tests();
    return UNITY_END();
}
