/*
 * Generated configuration tables: checks that apply to every generated build
 * (DP-WP4 D3, Test Spec GEN-003/GEN-004/GEN-006, CFG-016).
 *
 * Linked once with main/generated (lab_bench) and once with
 * test/host/generated/decoder_test. Both define 'ems_config_build'.
 */
#include <string.h>
#include "unity.h"
#include "ems_config_gen.h"
#include "ems_measurement.h"
#include "ems_register_map.h"

static const ems_config_build_t *const B = &ems_config_build;

/* GEN-004: the CRC stored by the tool equals the CRC the firmware computes. */
static void test_GEN004_stored_crc_matches_firmware_crc(void)
{
    TEST_ASSERT_EQUAL_HEX32(EMS_CONFIG_GEN_CRC32, B->crc32);
    TEST_ASSERT_EQUAL_HEX32(B->crc32, ems_config_crc32(B));
}

/* GEN-004: a change in any table field changes the CRC. */
static void test_GEN004_crc_detects_table_changes(void)
{
    ems_config_build_t b = *B;
    ems_cfg_map_t maps[EMS_CFG_MAX_MAPS];
    ems_reg_mapping_t mp[EMS_CFG_MAX_MAPPINGS_PER_MAP];
    ems_cfg_device_t dev[EMS_CFG_MAX_DEVICES];
    const uint32_t good = ems_config_crc32(B);

    TEST_ASSERT_TRUE(B->map_count >= 1u && B->maps[0].mapping_count >= 1u);
    memcpy(maps, B->maps, B->map_count * sizeof(maps[0]));
    memcpy(mp, B->maps[0].mappings, B->maps[0].mapping_count * sizeof(mp[0]));
    memcpy(dev, B->devices, B->device_count * sizeof(dev[0]));
    maps[0].mappings = mp;
    b.maps = maps;
    b.devices = dev;
    TEST_ASSERT_EQUAL_HEX32(good, ems_config_crc32(&b));

    mp[0].scale *= 10.0;
    TEST_ASSERT_NOT_EQUAL(good, ems_config_crc32(&b));
    mp[0] = B->maps[0].mappings[0];
    mp[0].address = (uint16_t)(mp[0].address + 1u);
    TEST_ASSERT_NOT_EQUAL(good, ems_config_crc32(&b));
    mp[0] = B->maps[0].mappings[0];
    dev[0].ct_ratio = dev[0].ct_ratio + 1.0;
    TEST_ASSERT_NOT_EQUAL(good, ems_config_crc32(&b));
    dev[0] = B->devices[0];
    dev[0].slave_id = (uint8_t)(dev[0].slave_id + 1u);
    TEST_ASSERT_NOT_EQUAL(good, ems_config_crc32(&b));
    dev[0] = B->devices[0];
    TEST_ASSERT_EQUAL_HEX32(good, ems_config_crc32(&b));
    TEST_ASSERT_EQUAL_HEX32(0u, ems_config_crc32(NULL));
}

/* GEN-003 / CFG-016: the generated tables pass the firmware's own check. */
static void test_GEN003_every_map_passes_firmware_validation(void)
{
    for (size_t i = 0; i < B->map_count; i++) {
        const ems_cfg_map_t *m = &B->maps[i];
        size_t bad = 99u;
        ems_map_check_t r = ems_regmap_validate(m->groups, m->group_count,
                                                m->mappings, m->mapping_count, &bad);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(ems_map_check_name(EMS_MAP_OK), ems_map_check_name(r), m->map_id);
        TEST_ASSERT_TRUE(m->group_count <= EMS_CFG_MAX_GROUPS_PER_MAP);
        TEST_ASSERT_TRUE(m->mapping_count <= EMS_CFG_MAX_MAPPINGS_PER_MAP);
    }
}

/* GEN-003: every parameter ID exists in the dictionary and is unique per map. */
static void test_GEN003_parameter_ids_exist_in_dictionary(void)
{
    for (size_t i = 0; i < B->map_count; i++) {
        const ems_cfg_map_t *m = &B->maps[i];
        for (size_t k = 0; k < m->mapping_count; k++) {
            const ems_param_info_t *p = ems_param_find(m->mappings[k].param_id);
            TEST_ASSERT_NOT_NULL(p);
            const bool counter = (m->mappings[k].flags & EMS_MAP_F_COUNTER) != 0u;
            TEST_ASSERT_EQUAL(p->value_type == EMS_VT_COUNTER, counter);
            for (size_t j = k + 1u; j < m->mapping_count; j++) {
                TEST_ASSERT_NOT_EQUAL(m->mappings[k].param_id, m->mappings[j].param_id);
            }
        }
    }
}

/* GEN-006: all cross-references between the tables are in range. */
static void test_GEN006_table_references_are_consistent(void)
{
    TEST_ASSERT_EQUAL_UINT8(EMS_CONFIG_GEN_DEVICE_COUNT, B->device_count);
    TEST_ASSERT_EQUAL_STRING(EMS_CONFIG_GEN_SITE_ID, B->site_id);
    TEST_ASSERT_TRUE(B->device_count >= 1u && B->device_count <= EMS_CFG_MAX_DEVICES);
    TEST_ASSERT_TRUE(B->bus_count >= 1u && B->bus_count <= EMS_CFG_MAX_BUSES);
    TEST_ASSERT_TRUE(B->map_count >= 1u && B->map_count <= EMS_CFG_MAX_MAPS);
    TEST_ASSERT_EQUAL_UINT32(16u, (uint32_t)strlen(B->inputs_sha256));
    for (size_t i = 0; i < B->profile_count; i++) {
        TEST_ASSERT_TRUE(B->profiles[i].map_index < B->map_count);
        TEST_ASSERT_TRUE(B->profiles[i].classification <= B->classification);
    }
    for (size_t i = 0; i < B->device_count; i++) {
        const ems_cfg_device_t *d = &B->devices[i];
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, d->slot);
        TEST_ASSERT_TRUE(d->profile_index < B->profile_count);
        TEST_ASSERT_TRUE(d->bus_index < B->bus_count);
        TEST_ASSERT_TRUE(d->slave_id >= 1u && d->slave_id <= 247u);
        const ems_cfg_map_t *m = &B->maps[B->profiles[d->profile_index].map_index];
        TEST_ASSERT_TRUE(d->probe_group < m->group_count);
        TEST_ASSERT_TRUE(d->ct_ratio > 0.0 && d->pt_ratio > 0.0);
        for (size_t j = i + 1u; j < B->device_count; j++) {
            const bool same_bus = B->devices[j].bus_index == d->bus_index;
            TEST_ASSERT_FALSE(same_bus && B->devices[j].slave_id == d->slave_id);
        }
    }
    /* Simulation and test tables are never marked PRODUCTION (V-13). */
    if (B->target == EMS_TARGET_PROD) {
        TEST_ASSERT_EQUAL_UINT8(EMS_CLASS_PRODUCTION, B->classification);
    }
    TEST_ASSERT_NOT_EQUAL(0, strcmp("UNKNOWN", ems_classification_name(B->classification)));
}

void run_config_common_tests(void)
{
    RUN_TEST(test_GEN004_stored_crc_matches_firmware_crc);
    RUN_TEST(test_GEN004_crc_detects_table_changes);
    RUN_TEST(test_GEN003_every_map_passes_firmware_validation);
    RUN_TEST(test_GEN003_parameter_ids_exist_in_dictionary);
    RUN_TEST(test_GEN006_table_references_are_consistent);
}
