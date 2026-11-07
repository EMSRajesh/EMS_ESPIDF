/*
 * MyEMS host unit tests - runner (DP-WP4).
 * Test IDs in names refer to the DP-WP4 Verification & Test Specification v1.1.
 */
#include "unity.h"
#include "ems_state_store.h"

void run_dictionary_tests(void);
void run_state_store_tests(void);

void setUp(void)
{
    ems_state_deinit();
}

void tearDown(void)
{
    ems_state_deinit();
}

int main(void)
{
    UNITY_BEGIN();
    run_dictionary_tests();
    run_state_store_tests();
    return UNITY_END();
}
