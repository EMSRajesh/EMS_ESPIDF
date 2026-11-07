/*
 * ems_config_crc.c - canonical CRC-32 of the generated configuration (DP-WP4 4A)
 *
 * The serialisation order and field widths MUST match canonical_bytes() in
 * tools/profile/myems_profile_tool.py. Change both together.
 */
#include "ems_config_types.h"

#include <string.h>

uint32_t ems_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0u; i < len; ++i) {
        crc ^= p[i];
        for (unsigned b = 0u; b < 8u; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* Little-endian writers feeding the CRC directly (no buffer needed). */
static uint32_t put_u8(uint32_t crc, uint8_t v)
{
    return ems_crc32_update(crc, &v, 1u);
}

static uint32_t put_u16(uint32_t crc, uint16_t v)
{
    const uint8_t b[2] = { (uint8_t)(v & 0xFFu), (uint8_t)(v >> 8) };
    return ems_crc32_update(crc, b, 2u);
}

static uint32_t put_u32(uint32_t crc, uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu),
                           (uint8_t)((v >> 16) & 0xFFu), (uint8_t)(v >> 24) };
    return ems_crc32_update(crc, b, 4u);
}

static uint32_t put_f64(uint32_t crc, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    crc = put_u32(crc, (uint32_t)(bits & 0xFFFFFFFFu));
    return put_u32(crc, (uint32_t)(bits >> 32));
}

uint32_t ems_config_crc32(const ems_config_build_t *b)
{
    uint32_t crc = 0u;
    if (b == NULL) {
        return 0u;
    }
    crc = put_u8(crc, b->bus_count);
    crc = put_u8(crc, b->map_count);
    crc = put_u8(crc, b->profile_count);
    crc = put_u8(crc, b->device_count);
    crc = put_u8(crc, b->target);
    crc = put_u8(crc, b->classification);

    for (uint8_t i = 0u; i < b->bus_count; ++i) {
        const ems_cfg_bus_t *x = &b->buses[i];
        crc = put_u32(crc, x->baud);
        crc = put_u8(crc, x->uart_num);
        crc = put_u8(crc, x->tx_gpio);
        crc = put_u8(crc, x->rx_gpio);
        crc = put_u8(crc, x->rts_gpio);
        crc = put_u8(crc, x->parity);
        crc = put_u8(crc, x->stop_bits);
    }

    for (uint8_t i = 0u; i < b->map_count; ++i) {
        const ems_cfg_map_t *m = &b->maps[i];
        crc = put_u8(crc, m->group_count);
        crc = put_u16(crc, m->mapping_count);
        crc = put_u8(crc, m->classification);
        for (uint8_t g = 0u; g < m->group_count; ++g) {
            const ems_poll_group_t *gr = &m->groups[g];
            crc = put_u32(crc, gr->interval_ms);
            crc = put_u32(crc, gr->offset_ms);
            crc = put_u32(crc, gr->stale_timeout_ms);
            crc = put_u16(crc, gr->start_address);
            crc = put_u16(crc, gr->count);
            crc = put_u8(crc, gr->function_code);
        }
        for (uint16_t k = 0u; k < m->mapping_count; ++k) {
            const ems_reg_mapping_t *mp = &m->mappings[k];
            crc = put_f64(crc, mp->scale);
            crc = put_f64(crc, mp->offset);
            crc = put_f64(crc, mp->min);
            crc = put_f64(crc, mp->max);
            crc = put_u16(crc, mp->mapping_id);
            crc = put_u16(crc, mp->param_id);
            crc = put_u16(crc, mp->address);
            crc = put_u16(crc, mp->bit_mask);
            crc = put_u8(crc, mp->group_index);
            crc = put_u8(crc, mp->type);
            crc = put_u8(crc, mp->order);
            crc = put_u8(crc, mp->flags);
        }
    }

    for (uint8_t i = 0u; i < b->profile_count; ++i) {
        const ems_cfg_profile_t *p = &b->profiles[i];
        crc = put_f64(crc, p->counter_max_rate_per_hour);
        crc = put_u8(crc, p->map_index);
        crc = put_u8(crc, p->classification);
        crc = put_u8(crc, p->ratio_handling);
    }

    for (uint8_t i = 0u; i < b->device_count; ++i) {
        const ems_cfg_device_t *d = &b->devices[i];
        crc = put_f64(crc, d->ct_ratio);
        crc = put_f64(crc, d->pt_ratio);
        crc = put_u32(crc, d->probe_interval_ms);
        crc = put_u16(crc, d->timeout_ms);
        crc = put_u8(crc, d->slot);
        crc = put_u8(crc, d->profile_index);
        crc = put_u8(crc, d->bus_index);
        crc = put_u8(crc, d->slave_id);
        crc = put_u8(crc, d->max_retries);
        crc = put_u8(crc, d->comm_lost_threshold);
        crc = put_u8(crc, d->probe_group);
        crc = put_u8(crc, d->enabled ? 1u : 0u);
    }
    return crc;
}

const char *ems_classification_name(uint8_t classification)
{
    switch (classification) {
    case EMS_CLASS_PRODUCTION:      return "PRODUCTION";
    case EMS_CLASS_SIMULATION_ONLY: return "SIMULATION_ONLY";
    case EMS_CLASS_TEST_ONLY:       return "TEST_ONLY";
    default:                        return "UNKNOWN";
    }
}
