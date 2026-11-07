/*
 * MyEMS host unit tests - register map engine runner (DP-WP4 D2).
 * Test IDs in names refer to the DP-WP4 Verification & Test Specification v1.1.
 */
#include "unity.h"

void run_register_map_tests(void);
void run_counter_tests(void);

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    run_register_map_tests();
    run_counter_tests();
    return UNITY_END();
}
