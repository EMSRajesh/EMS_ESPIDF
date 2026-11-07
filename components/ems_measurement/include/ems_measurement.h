/*
 * ems_measurement.h - MyEMS Universal Measurement Dictionary (DP-WP4 4B)
 *
 * Common types for normalized measurements: status codes, quality, units,
 * value types, the measurement record, and dictionary lookup.
 *
 * Pure C11. No ESP-IDF or FreeRTOS dependency (Design DR-23), so the same
 * code builds for the ESP32-S3 and for host unit tests.
 *
 * Design reference: DP-WP4 Design Specification v1.1, Section 6.
 */
#ifndef EMS_MEASUREMENT_H
#define EMS_MEASUREMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ems_param_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time assertion usable in C11 compilers including MSVC. */
#if defined(__cplusplus)
#define EMS_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define EMS_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* Result codes of the pure-C DP-WP4 modules. */
typedef enum {
    EMS_OK = 0,
    EMS_ERR_INVALID_ARG,    /* NULL pointer, bad slot, non-finite value, ... */
    EMS_ERR_NOT_FOUND,      /* parameter ID not in the dictionary            */
    EMS_ERR_NO_SPACE,       /* layout larger than the static limits          */
    EMS_ERR_INVALID_STATE   /* not initialised, or device disabled           */
} ems_status_t;

/*
 * Measurement quality (Design 6.2).
 * Rule for every consumer: use the value only when quality == EMS_Q_GOOD.
 * Any other quality means "unknown" - never zero.
 */
typedef enum {
    EMS_Q_INITIALIZING = 0, /* no sample since activation                       */
    EMS_Q_GOOD,             /* fresh, decoded, in range                         */
    EMS_Q_STALE,            /* last poll failed or too old; device ONLINE       */
    EMS_Q_COMM_LOST,        /* device OFFLINE                                   */
    EMS_Q_INVALID,          /* decode failed or Modbus exception                */
    EMS_Q_OUT_OF_RANGE,     /* decoded value outside configured min/max         */
    EMS_Q_NOT_SUPPORTED,    /* capability absent or device disabled             */
    EMS_Q_COUNT
} ems_quality_t;

/* Canonical units (Design 6.1). Order is part of the generated-table ABI. */
typedef enum {
    EMS_UNIT_NONE = 0,  /* "-"    */
    EMS_UNIT_V,         /* "V"    */
    EMS_UNIT_A,         /* "A"    */
    EMS_UNIT_KW,        /* "kW"   */
    EMS_UNIT_KVAR,      /* "kvar" */
    EMS_UNIT_KVA,       /* "kVA"  */
    EMS_UNIT_RATIO,     /* "1"    */
    EMS_UNIT_HZ,        /* "Hz"   */
    EMS_UNIT_KWH,       /* "kWh"  */
    EMS_UNIT_COUNT
} ems_unit_t;

typedef enum {
    EMS_VT_FLOAT = 0,   /* instantaneous engineering value          */
    EMS_VT_COUNTER,     /* cumulative, normally non-decreasing      */
    EMS_VT_ENUM,        /* status word or enumeration (as a number) */
    EMS_VT_COUNT
} ems_value_type_t;

/* One dictionary entry (generated into ems_param_table.c). */
typedef struct {
    uint16_t         runtime_id;
    ems_unit_t       unit;
    ems_value_type_t value_type;
    const char      *name;         /* string ID, e.g. "ac.voltage.l1_n" */
    const char      *description;
} ems_param_info_t;

/*
 * Normalized measurement record (Design 6.3). 40 bytes, fixed layout.
 * sample_mono_us / sample_utc_ms always describe the last GOOD sample
 * (0 if there never was one), so the age of the data is always known.
 */
typedef struct {
    uint16_t param_id;       /* runtime ID                                   */
    uint8_t  device_slot;    /* index into the site device table             */
    uint8_t  quality;        /* ems_quality_t                                */
    uint32_t seq;            /* +1 on every change of this record            */
    double   value;          /* engineering value in the canonical unit      */
    int64_t  sample_mono_us; /* monotonic time of last GOOD sample, 0 = none */
    int64_t  sample_utc_ms;  /* UTC of last GOOD sample when utc_valid       */
    uint8_t  utc_valid;      /* 0 in DP-WP4 (no time service yet)            */
    uint8_t  reserved[7];
} ems_measurement_record_t;

EMS_STATIC_ASSERT(sizeof(ems_measurement_record_t) == 40u,
                  "ems_measurement_record_t must be 40 bytes (Design 6.3)");
EMS_STATIC_ASSERT(offsetof(ems_measurement_record_t, value) == 8u,
                  "value must be 8-byte aligned at offset 8");

/* Generated dictionary table, sorted by runtime ID. */
extern const ems_param_info_t ems_param_table[EMS_PARAM_COUNT];
extern const size_t ems_param_table_len;

/* Dictionary lookup. Return NULL when not found. */
const ems_param_info_t *ems_param_find(uint16_t runtime_id);
const ems_param_info_t *ems_param_find_by_name(const char *name);

/* True only for EMS_Q_GOOD: the single test consumers must apply. */
static inline bool ems_quality_is_usable(ems_quality_t quality)
{
    return quality == EMS_Q_GOOD;
}

/* Text helpers for logs and diagnostics. Never return NULL. */
const char *ems_quality_name(ems_quality_t quality);
const char *ems_unit_symbol(ems_unit_t unit);
const char *ems_status_name(ems_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* EMS_MEASUREMENT_H */
