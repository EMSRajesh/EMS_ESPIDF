/*
 * MyEMS host unit tests - generated configuration, decoder test site (DP-WP4 D3).
 * Uses test/host/generated/decoder_test/ems_config_gen.c.
 */
#include "unity.h"

void run_config_common_tests(void);
void run_config_decoder_tests(void);

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
    run_config_decoder_tests();
    return UNITY_END();
}
