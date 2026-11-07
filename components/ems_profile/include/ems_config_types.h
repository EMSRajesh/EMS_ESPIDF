/*
 * ems_config_types.h - generated configuration tables (DP-WP4 4A)
 *
 * tools/profile/myems_profile_tool.py turns the profile, register map and
 * site JSON files into const tables of these types
 * (main/generated/ems_config_gen.c). The firmware never parses JSON.
 *
 * Pure C11, no ESP-IDF dependency (DR-23). Design reference: DP-WP4 Design
 * Specification v1.1, Section 5.
 */
#ifndef EMS_CONFIG_TYPES_H
#define EMS_CONFIG_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ems_register_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware table limits (rule V-15). The host tool checks the same values. */
#define EMS_CFG_MAX_BUSES          2u
#define EMS_CFG_MAX_DEVICES        4u
#define EMS_CFG_MAX_MAPS           4u
#define EMS_CFG_MAX_GROUPS_PER_MAP 8u
#define EMS_CFG_MAX_POLL_ENTRIES  16u   /* sum of poll groups over enabled devices */
#define EMS_CFG_MAX_MAPPINGS_PER_MAP 32u
#define EMS_CFG_MAX_MAPPINGS     128u

typedef enum {
    EMS_CLASS_PRODUCTION = 0,
    EMS_CLASS_SIMULATION_ONLY = 1,
    EMS_CLASS_TEST_ONLY = 2
} ems_classification_t;

typedef enum {
    EMS_TARGET_PROD = 0,
    EMS_TARGET_SIM = 1
} ems_config_target_t;

typedef enum {
    EMS_RATIO_MYEMS_APPLIES = 0,
    EMS_RATIO_METER_REPORTS_PRIMARY = 1
} ems_ratio_handling_t;

typedef enum {
    EMS_PARITY_NONE = 0,
    EMS_PARITY_EVEN = 1,
    EMS_PARITY_ODD = 2
} ems_parity_t;

/* One RS-485 bus. Index in ems_config_build_t.buses = bus index. */
typedef struct {
    const char *bus_id;
    uint32_t baud;
    uint8_t  uart_num;
    uint8_t  tx_gpio;
    uint8_t  rx_gpio;
    uint8_t  rts_gpio;
    uint8_t  parity;       /* ems_parity_t */
    uint8_t  stop_bits;    /* 1 or 2 */
} ems_cfg_bus_t;

/* One register map: poll groups + mappings (shared by all devices using it). */
typedef struct {
    const char *map_id;
    const char *map_version;
    const ems_poll_group_t *groups;
    const ems_reg_mapping_t *mappings;
    uint16_t mapping_count;
    uint8_t  group_count;
    uint8_t  classification;   /* ems_classification_t */
} ems_cfg_map_t;

/* One device profile (type level). */
typedef struct {
    const char *profile_id;
    const char *profile_version;
    const char *device_type;
    double   counter_max_rate_per_hour;  /* plausibility limit for counter rollover */
    uint8_t  map_index;                  /* into ems_config_build_t.maps */
    uint8_t  classification;             /* ems_classification_t */
    uint8_t  ratio_handling;             /* ems_ratio_handling_t */
} ems_cfg_profile_t;

/* One installed device (site level). Slot = index in ems_config_build_t.devices.
 * Communication values are resolved: profile defaults + site overrides. */
typedef struct {
    const char *instance_id;
    const char *location;
    double   ct_ratio;
    double   pt_ratio;
    uint32_t probe_interval_ms;
    uint16_t timeout_ms;
    uint8_t  slot;
    uint8_t  profile_index;        /* into ems_config_build_t.profiles */
    uint8_t  bus_index;            /* into ems_config_build_t.buses */
    uint8_t  slave_id;             /* 1..247 */
    uint8_t  max_retries;
    uint8_t  comm_lost_threshold;
    uint8_t  probe_group;          /* index into the device's map groups */
    bool     enabled;
} ems_cfg_device_t;

/* Everything the generator produced, plus identification. */
typedef struct {
    const char *site_id;
    const char *tool_version;
    const char *umd_version;
    const char *inputs_sha256;     /* first 16 hex digits over all input files */
    const char *inputs_summary;    /* "file@version; ..." */
    const ems_cfg_bus_t     *buses;
    const ems_cfg_map_t     *maps;
    const ems_cfg_profile_t *profiles;
    const ems_cfg_device_t  *devices;
    uint32_t crc32;                /* ems_config_crc32() of the tables */
    uint8_t  bus_count;
    uint8_t  map_count;
    uint8_t  profile_count;
    uint8_t  device_count;
    uint8_t  target;               /* ems_config_target_t */
    uint8_t  classification;       /* highest of all profiles and maps */
} ems_config_build_t;

/*
 * CRC-32 (IEEE 802.3, the same as Python zlib.crc32) over a canonical,
 * layout-independent serialisation of all numeric table fields
 * (little-endian, doubles as IEEE-754 bit patterns, strings excluded).
 * Struct padding and byte order of the target do not affect the result, so
 * the value computed by the host tool matches the firmware.
 */
uint32_t ems_config_crc32(const ems_config_build_t *build);

/* Incremental CRC-32: start with crc = 0. */
uint32_t ems_crc32_update(uint32_t crc, const void *data, size_t len);

const char *ems_classification_name(uint8_t classification);

#ifdef __cplusplus
}
#endif

#endif /* EMS_CONFIG_TYPES_H */
