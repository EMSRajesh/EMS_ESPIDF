/*
 * MyEMS host unit tests - per-site generated tables (DP-WP4 D9).
 * Built once per site in main/generated_sites/ (see CMakeLists.txt).
 */
#include "unity.h"

void run_config_common_tests(void);
void run_config_site_tests(void);

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
    run_config_site_tests();
    return UNITY_END();
}
