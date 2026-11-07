/*
 * ems_register_map.h - MyEMS Modbus register map engine (DP-WP4 4C)
 *
 * Turns the raw 16-bit register words of one poll-group response into
 * engineering values: extract -> decode (type, byte/word order) -> scale and
 * offset -> CT/PT ratio -> range check. Counter tracking is in ems_counter.h.
 *
 * Pure C11, no ESP-IDF/FreeRTOS dependency, no dynamic memory (DR-20, DR-23).
 * Design reference: DP-WP4 Design Specification v1.1, Section 7.
 *
 * Register words are as delivered by modbus_rtu_master: register n is
 * (first byte on the wire << 8) | second byte.
 */
#ifndef EMS_REGISTER_MAP_H
#define EMS_REGISTER_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest FC03/FC04 read (Modbus limit). */
#define EMS_REGMAP_MAX_GROUP_REGS 125u

/* Data types (Design 7.1). */
typedef enum {
    EMS_REG_UINT16 = 0,
    EMS_REG_INT16,
    EMS_REG_UINT32,
    EMS_REG_INT32,
    EMS_REG_FLOAT32,
    EMS_REG_BITFIELD16,
    EMS_REG_TYPE_COUNT
} ems_reg_type_t;

/*
 * Byte/word order. A is the most significant byte of the value.
 *   ABCD  reg n = A B, reg n+1 = C D   big-endian (Modbus default)
 *   CDAB  reg n = C D, reg n+1 = A B   word-swapped
 *   BADC  reg n = B A, reg n+1 = D C   byte-swapped
 *   DCBA  reg n = D C, reg n+1 = B A   little-endian
 * 16-bit types accept only AB (= ABCD) and BA (= BADC).
 */
typedef enum {
    EMS_ORDER_ABCD = 0,
    EMS_ORDER_CDAB,
    EMS_ORDER_BADC,
    EMS_ORDER_DCBA,
    EMS_ORDER_COUNT
} ems_word_order_t;

#define EMS_ORDER_AB EMS_ORDER_ABCD
#define EMS_ORDER_BA EMS_ORDER_BADC

/* Mapping flags. */
#define EMS_MAP_F_APPLY_CT  0x01u   /* multiply by site CT ratio            */
#define EMS_MAP_F_APPLY_PT  0x02u   /* multiply by site PT ratio            */
#define EMS_MAP_F_COUNTER   0x04u   /* cumulative counter (ems_counter.h)   */
#define EMS_MAP_F_ROLLOVER  0x08u   /* counter may wrap at 2^32 (UINT32)    */

/*
 * One register-to-parameter mapping (generated from the map JSON).
 * scale/offset/min/max are double: float cannot hold 0.01 kWh resolution on
 * large energy counters (CONV-007). 48 bytes.
 */
typedef struct {
    double   scale;        /* finite, non-zero                          */
    double   offset;       /* finite                                    */
    double   min;          /* inclusive, after ratio                    */
    double   max;          /* inclusive, after ratio                    */
    uint16_t mapping_id;
    uint16_t param_id;     /* runtime ID from the measurement dictionary */
    uint16_t address;      /* PDU address (0-based)                     */
    uint16_t bit_mask;     /* BITFIELD16 only; 0 = whole word           */
    uint8_t  group_index;  /* index into the device's poll groups       */
    uint8_t  type;         /* ems_reg_type_t                            */
    uint8_t  order;        /* ems_word_order_t                          */
    uint8_t  flags;        /* EMS_MAP_F_*                               */
} ems_reg_mapping_t;

/* One poll group: a contiguous register block read in one request. */
typedef struct {
    uint32_t interval_ms;
    uint32_t offset_ms;         /* first poll after activation */
    uint32_t stale_timeout_ms;
    uint16_t start_address;
    uint16_t count;             /* 1 .. EMS_REGMAP_MAX_GROUP_REGS */
    uint8_t  function_code;     /* 3 or 4 (read-only, DR-21) */
} ems_poll_group_t;

/* Site CT/PT ratios. Pass NULL to ems_regmap_convert() when the meter
 * already reports primary values (ratio_handling = METER_REPORTS_PRIMARY). */
typedef struct {
    double ct;
    double pt;
} ems_ratio_t;

typedef enum {
    EMS_DEC_OK = 0,          /* value valid and in range              */
    EMS_DEC_OUT_OF_RANGE,    /* value valid but outside min/max       */
    EMS_DEC_INVALID          /* no usable value (bounds, NaN/Inf, ...) */
} ems_decode_status_t;

typedef struct {
    double   value;     /* engineering value; set for OK and OUT_OF_RANGE */
    int64_t  raw;       /* decoded integer (sign-extended); 0 for FLOAT32  */
    uint32_t raw_bits;  /* raw register bits as unsigned 32-bit            */
} ems_regmap_result_t;

/* Number of registers occupied by a type: 1, 2, or 0 if the type is unknown. */
uint8_t ems_regmap_type_regs(uint8_t type);

/* True if 'order' is allowed for 'type'. */
bool ems_regmap_order_valid(uint8_t type, uint8_t order);

/*
 * A response must contain exactly the registers of its poll group. Anything
 * else means the whole group is INVALID and nothing is published (DEC-009).
 */
bool ems_regmap_response_complete(const ems_poll_group_t *group, uint16_t received_count);

/*
 * Decode step only (Design 7.3 steps 1-2): extract and decode the raw value,
 * without scale, ratio or range. out->value holds the unscaled number.
 * Returns EMS_DEC_INVALID for out-of-bounds, bad type/order or a non-finite
 * FLOAT32; EMS_DEC_OK otherwise.
 */
ems_decode_status_t ems_regmap_decode(const ems_reg_mapping_t *map,
                                      const ems_poll_group_t *group,
                                      const uint16_t *regs, uint16_t reg_count,
                                      ems_regmap_result_t *out);

/*
 * Full conversion (Design 7.3 steps 1-5): extract, decode, scale/offset,
 * ratio (only if ratio != NULL and the mapping has APPLY_CT/APPLY_PT),
 * range check. out->value is set for OK and OUT_OF_RANGE.
 */
ems_decode_status_t ems_regmap_convert(const ems_reg_mapping_t *map,
                                       const ems_poll_group_t *group,
                                       const uint16_t *regs, uint16_t reg_count,
                                       const ems_ratio_t *ratio,
                                       ems_regmap_result_t *out);

/* Ratio factor a mapping gets: 1, CT, PT or CT x PT. */
double ems_regmap_ratio_factor(const ems_reg_mapping_t *map, const ems_ratio_t *ratio);

/* ---- Structural validation (firmware side of rules V-05..V-08) ---------- */

typedef enum {
    EMS_MAP_OK = 0,
    EMS_MAP_ERR_NULL,
    EMS_MAP_ERR_GROUP_FC,        /* function code not 3/4             */
    EMS_MAP_ERR_GROUP_COUNT,     /* count 0, > 125, or past 65535     */
    EMS_MAP_ERR_GROUP_TIMING,    /* interval < 1000 or stale <= interval */
    EMS_MAP_ERR_GROUP_REF,       /* group_index out of range          */
    EMS_MAP_ERR_TYPE,            /* unknown type                      */
    EMS_MAP_ERR_ORDER,           /* order not allowed for the type    */
    EMS_MAP_ERR_OUTSIDE_BLOCK,   /* registers not inside group block  */
    EMS_MAP_ERR_SCALE,           /* scale zero/non-finite, offset non-finite */
    EMS_MAP_ERR_RANGE,           /* min >= max or non-finite          */
    EMS_MAP_ERR_MASK,            /* bit_mask on a non-BITFIELD16 type */
    EMS_MAP_ERR_FLAGS,           /* counter flags on unsuitable type  */
    EMS_MAP_ERR_DUPLICATE_ID     /* mapping_id used twice             */
} ems_map_check_t;

/*
 * Check poll groups and mappings of one register map. On error, *bad_index
 * (if not NULL) receives the index of the offending group or mapping.
 */
ems_map_check_t ems_regmap_validate(const ems_poll_group_t *groups, size_t group_count,
                                    const ems_reg_mapping_t *maps, size_t map_count,
                                    size_t *bad_index);

const char *ems_map_check_name(ems_map_check_t check);

#ifdef __cplusplus
}
#endif

#endif /* EMS_REGISTER_MAP_H */
