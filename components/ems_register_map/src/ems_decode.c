/*
 * ems_decode.c - register extraction and type decoding (DP-WP4 4C)
 */
#include "ems_register_map.h"

#include <math.h>
#include <string.h>

static uint16_t swap16(uint16_t w)
{
    return (uint16_t)((uint16_t)(w << 8) | (uint16_t)(w >> 8));
}

uint8_t ems_regmap_type_regs(uint8_t type)
{
    switch ((ems_reg_type_t)type) {
    case EMS_REG_UINT16:
    case EMS_REG_INT16:
    case EMS_REG_BITFIELD16:
        return 1u;
    case EMS_REG_UINT32:
    case EMS_REG_INT32:
    case EMS_REG_FLOAT32:
        return 2u;
    case EMS_REG_TYPE_COUNT:
    default:
        return 0u;
    }
}

bool ems_regmap_order_valid(uint8_t type, uint8_t order)
{
    const uint8_t regs = ems_regmap_type_regs(type);
    if (regs == 1u) {
        return order == (uint8_t)EMS_ORDER_AB || order == (uint8_t)EMS_ORDER_BA;
    }
    if (regs == 2u) {
        return order < (uint8_t)EMS_ORDER_COUNT;
    }
    return false;
}

bool ems_regmap_response_complete(const ems_poll_group_t *group, uint16_t received_count)
{
    return group != NULL && group->count > 0u && received_count == group->count;
}

/* Assemble a 32-bit value (A = most significant byte) from two registers. */
static uint32_t assemble32(uint16_t r0, uint16_t r1, uint8_t order)
{
    switch ((ems_word_order_t)order) {
    case EMS_ORDER_CDAB:
        return ((uint32_t)r1 << 16) | r0;
    case EMS_ORDER_BADC:
        return ((uint32_t)swap16(r0) << 16) | swap16(r1);
    case EMS_ORDER_DCBA:
        return ((uint32_t)swap16(r1) << 16) | swap16(r0);
    case EMS_ORDER_ABCD:
    case EMS_ORDER_COUNT:
    default:
        return ((uint32_t)r0 << 16) | r1;
    }
}

/* Lowest set bit position of a non-zero mask. */
static unsigned mask_shift(uint16_t mask)
{
    unsigned shift = 0u;
    while (shift < 16u && ((mask >> shift) & 1u) == 0u) {
        shift++;
    }
    return shift;
}

ems_decode_status_t ems_regmap_decode(const ems_reg_mapping_t *map,
                                      const ems_poll_group_t *group,
                                      const uint16_t *regs, uint16_t reg_count,
                                      ems_regmap_result_t *out)
{
    if (map == NULL || group == NULL || regs == NULL || out == NULL) {
        return EMS_DEC_INVALID;
    }
    memset(out, 0, sizeof(*out));

    const uint8_t nregs = ems_regmap_type_regs(map->type);
    if (nregs == 0u || !ems_regmap_order_valid(map->type, map->order)) {
        return EMS_DEC_INVALID;
    }

    /* Step 1: extract. The mapping must lie inside the received registers. */
    if (map->address < group->start_address) {
        return EMS_DEC_INVALID;
    }
    const uint32_t index = (uint32_t)map->address - group->start_address;
    if (index + nregs > reg_count) {
        return EMS_DEC_INVALID;
    }
    const uint16_t r0 = regs[index];

    /* Step 2: decode. */
    switch ((ems_reg_type_t)map->type) {
    case EMS_REG_UINT16: {
        const uint16_t w = (map->order == (uint8_t)EMS_ORDER_BA) ? swap16(r0) : r0;
        out->raw_bits = w;
        out->raw = (int64_t)w;
        out->value = (double)w;
        return EMS_DEC_OK;
    }
    case EMS_REG_INT16: {
        const uint16_t w = (map->order == (uint8_t)EMS_ORDER_BA) ? swap16(r0) : r0;
        /* Explicit sign extension: portable, no implementation-defined cast. */
        const int32_t s = ((w & 0x8000u) != 0u) ? (int32_t)w - 65536 : (int32_t)w;
        out->raw_bits = w;
        out->raw = (int64_t)s;
        out->value = (double)s;
        return EMS_DEC_OK;
    }
    case EMS_REG_BITFIELD16: {
        const uint16_t w = (map->order == (uint8_t)EMS_ORDER_BA) ? swap16(r0) : r0;
        uint16_t field = w;
        if (map->bit_mask != 0u) {
            field = (uint16_t)((w & map->bit_mask) >> mask_shift(map->bit_mask));
        }
        out->raw_bits = w;
        out->raw = (int64_t)field;
        out->value = (double)field;
        return EMS_DEC_OK;
    }
    case EMS_REG_UINT32: {
        const uint32_t u = assemble32(r0, regs[index + 1u], map->order);
        out->raw_bits = u;
        out->raw = (int64_t)u;
        out->value = (double)u;
        return EMS_DEC_OK;
    }
    case EMS_REG_INT32: {
        const uint32_t u = assemble32(r0, regs[index + 1u], map->order);
        const int64_t s = ((u & 0x80000000u) != 0u) ? (int64_t)u - 4294967296LL : (int64_t)u;
        out->raw_bits = u;
        out->raw = (int64_t)s;
        out->value = (double)s;
        return EMS_DEC_OK;
    }
    case EMS_REG_FLOAT32: {
        const uint32_t u = assemble32(r0, regs[index + 1u], map->order);
        float f;
        memcpy(&f, &u, sizeof(f));             /* IEEE-754 binary32 */
        out->raw_bits = u;
        out->raw = 0;
        if (!isfinite(f)) {
            return EMS_DEC_INVALID;            /* NaN, +Inf, -Inf (DEC-007) */
        }
        out->value = (double)f;
        return EMS_DEC_OK;
    }
    case EMS_REG_TYPE_COUNT:
    default:
        return EMS_DEC_INVALID;
    }
}
