/*
 * MyEMS host unit tests - generated configuration, lab_bench site (DP-WP4 D3).
 * Uses main/generated/ems_config_gen.c as committed in the repository.
 */
#include "unity.h"

void run_config_common_tests(void);
void run_config_lab_tests(void);

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    run_config_common_tests();
    run_config_lab_tests();
    return UNITY_END();
}
