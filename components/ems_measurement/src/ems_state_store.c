/*
 * ems_state_store.c - MyEMS measurement state store (DP-WP4 4B)
 *
 * See ems_state_store.h for ownership and quality rules.
 */
#include "ems_state_store.h"

#include <math.h>
#include <string.h>

typedef struct {
    ems_measurement_record_t rec;
    uint32_t stale_timeout_ms;
} entry_t;

typedef struct {
    entry_t entries[EMS_MAX_PARAMS_PER_DEVICE];
    uint8_t count;
    bool    enabled;
} device_t;

static device_t s_devices[EMS_MAX_DEVICES];
static uint8_t  s_device_count;
static bool     s_initialised;
static ems_state_lock_t s_lock;

/* ---------------------------------------------------------------- helpers */

static void lock(void)
{
    if (s_lock.lock != NULL) {
        s_lock.lock(s_lock.ctx);
    }
}

static void unlock(void)
{
    if (s_lock.unlock != NULL) {
        s_lock.unlock(s_lock.ctx);
    }
}

static entry_t *find_entry(uint8_t slot, uint16_t param_id)
{
    device_t *dev = &s_devices[slot];
    for (uint8_t i = 0u; i < dev->count; ++i) {
        if (dev->entries[i].rec.param_id == param_id) {
            return &dev->entries[i];
        }
    }
    return NULL;
}

/* Common argument checks for writer calls on one parameter. */
static ems_status_t writer_lookup(uint8_t slot, uint16_t param_id, entry_t **out)
{
    if (!s_initialised) {
        return EMS_ERR_INVALID_STATE;
    }
    if (slot >= s_device_count) {
        return EMS_ERR_INVALID_ARG;
    }
    if (ems_param_find(param_id) == NULL) {
        return EMS_ERR_NOT_FOUND;
    }
    if (!s_devices[slot].enabled) {
        return EMS_ERR_INVALID_STATE;
    }
    entry_t *e = find_entry(slot, param_id);
    if (e == NULL) {
        /* In the dictionary but not declared for this device. */
        return EMS_ERR_NOT_FOUND;
    }
    *out = e;
    return EMS_OK;
}

static void set_quality(entry_t *e, ems_quality_t q)
{
    if (e->rec.quality != (uint8_t)q) {
        e->rec.quality = (uint8_t)q;
        e->rec.seq++;
    }
}

/* ------------------------------------------------------------ lifecycle */

ems_status_t ems_state_init(const ems_state_layout_t *layout, const ems_state_lock_t *lock_cfg)
{
    if (s_initialised) {
        return EMS_ERR_INVALID_STATE;
    }
    if (layout == NULL || layout->devices == NULL || layout->device_count == 0u) {
        return EMS_ERR_INVALID_ARG;
    }
    if (layout->device_count > EMS_MAX_DEVICES) {
        return EMS_ERR_NO_SPACE;
    }

    /* Validate everything before touching the static state. */
    for (uint8_t d = 0u; d < layout->device_count; ++d) {
        const ems_state_device_def_t *def = &layout->devices[d];
        if (def->param_count > EMS_MAX_PARAMS_PER_DEVICE) {
            return EMS_ERR_NO_SPACE;
        }
        if (def->param_count > 0u && def->params == NULL) {
            return EMS_ERR_INVALID_ARG;
        }
        for (uint8_t p = 0u; p < def->param_count; ++p) {
            if (ems_param_find(def->params[p].param_id) == NULL) {
                return EMS_ERR_NOT_FOUND;
            }
            if (def->params[p].stale_timeout_ms == 0u) {
                return EMS_ERR_INVALID_ARG;
            }
            for (uint8_t q = 0u; q < p; ++q) {
                if (def->params[q].param_id == def->params[p].param_id) {
                    return EMS_ERR_INVALID_ARG;
                }
            }
        }
    }

    memset(s_devices, 0, sizeof(s_devices));
    for (uint8_t d = 0u; d < layout->device_count; ++d) {
        const ems_state_device_def_t *def = &layout->devices[d];
        device_t *dev = &s_devices[d];
        dev->count = def->param_count;
        dev->enabled = def->enabled;
        for (uint8_t p = 0u; p < def->param_count; ++p) {
            entry_t *e = &dev->entries[p];
            e->rec.param_id = def->params[p].param_id;
            e->rec.device_slot = d;
            e->rec.quality = (uint8_t)(def->enabled ? EMS_Q_INITIALIZING : EMS_Q_NOT_SUPPORTED);
            e->stale_timeout_ms = def->params[p].stale_timeout_ms;
        }
    }
    s_device_count = layout->device_count;
    if (lock_cfg != NULL) {
        s_lock = *lock_cfg;
    } else {
        memset(&s_lock, 0, sizeof(s_lock));
    }
    s_initialised = true;
    return EMS_OK;
}

void ems_state_deinit(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    memset(&s_lock, 0, sizeof(s_lock));
    s_device_count = 0u;
    s_initialised = false;
}

bool ems_state_is_initialised(void)
{
    return s_initialised;
}

/* --------------------------------------------------------------- writer */

ems_status_t ems_state_publish(uint8_t slot, uint16_t param_id, double value,
                               ems_quality_t quality, int64_t now_mono_us)
{
    if (quality != EMS_Q_GOOD && quality != EMS_Q_OUT_OF_RANGE) {
        return EMS_ERR_INVALID_ARG;
    }
    if (!isfinite(value)) {
        return EMS_ERR_INVALID_ARG;
    }
    entry_t *e = NULL;
    const ems_status_t st = writer_lookup(slot, param_id, &e);
    if (st != EMS_OK) {
        return st;
    }

    lock();
    e->rec.value = value;
    e->rec.quality = (uint8_t)quality;
    if (quality == EMS_Q_GOOD) {
        e->rec.sample_mono_us = now_mono_us;
        /* No time service in DP-WP4: UTC stays invalid. */
        e->rec.sample_utc_ms = 0;
        e->rec.utc_valid = 0u;
    }
    e->rec.seq++;
    unlock();
    return EMS_OK;
}

ems_status_t ems_state_mark_failed(uint8_t slot, uint16_t param_id)
{
    entry_t *e = NULL;
    const ems_status_t st = writer_lookup(slot, param_id, &e);
    if (st != EMS_OK) {
        return st;
    }
    lock();
    if (e->rec.quality == (uint8_t)EMS_Q_GOOD) {
        set_quality(e, EMS_Q_STALE);
    }
    unlock();
    return EMS_OK;
}

ems_status_t ems_state_mark_invalid(uint8_t slot, uint16_t param_id)
{
    entry_t *e = NULL;
    const ems_status_t st = writer_lookup(slot, param_id, &e);
    if (st != EMS_OK) {
        return st;
    }
    lock();
    set_quality(e, EMS_Q_INVALID);
    unlock();
    return EMS_OK;
}

ems_status_t ems_state_set_device_comm_lost(uint8_t slot)
{
    if (!s_initialised) {
        return EMS_ERR_INVALID_STATE;
    }
    if (slot >= s_device_count) {
        return EMS_ERR_INVALID_ARG;
    }
    device_t *dev = &s_devices[slot];
    if (!dev->enabled) {
        return EMS_ERR_INVALID_STATE;
    }
    lock();
    for (uint8_t i = 0u; i < dev->count; ++i) {
        set_quality(&dev->entries[i], EMS_Q_COMM_LOST);
    }
    unlock();
    return EMS_OK;
}

uint32_t ems_state_check_freshness(int64_t now_mono_us)
{
    uint32_t changed = 0u;
    if (!s_initialised) {
        return 0u;
    }
    lock();
    for (uint8_t d = 0u; d < s_device_count; ++d) {
        device_t *dev = &s_devices[d];
        for (uint8_t i = 0u; i < dev->count; ++i) {
            entry_t *e = &dev->entries[i];
            if (e->rec.quality != (uint8_t)EMS_Q_GOOD) {
                continue;
            }
            const int64_t age_us = now_mono_us - e->rec.sample_mono_us;
            if (age_us > (int64_t)e->stale_timeout_ms * 1000) {
                set_quality(e, EMS_Q_STALE);
                changed++;
            }
        }
    }
    unlock();
    return changed;
}

/* --------------------------------------------------------------- reader */

ems_status_t ems_state_get(uint8_t slot, uint16_t param_id, ems_measurement_record_t *out)
{
    if (out == NULL) {
        return EMS_ERR_INVALID_ARG;
    }
    if (!s_initialised) {
        return EMS_ERR_INVALID_STATE;
    }
    if (slot >= s_device_count) {
        return EMS_ERR_INVALID_ARG;
    }
    if (ems_param_find(param_id) == NULL) {
        return EMS_ERR_NOT_FOUND;
    }

    lock();
    const entry_t *e = find_entry(slot, param_id);
    if (e != NULL) {
        *out = e->rec;
    }
    unlock();

    if (e == NULL) {
        memset(out, 0, sizeof(*out));
        out->param_id = param_id;
        out->device_slot = slot;
        out->quality = (uint8_t)EMS_Q_NOT_SUPPORTED;
    }
    return EMS_OK;
}

ems_status_t ems_state_snapshot_device(uint8_t slot, ems_measurement_record_t *out,
                                       size_t max_records, size_t *out_count)
{
    if (out == NULL || out_count == NULL) {
        return EMS_ERR_INVALID_ARG;
    }
    *out_count = 0u;
    if (!s_initialised) {
        return EMS_ERR_INVALID_STATE;
    }
    if (slot >= s_device_count) {
        return EMS_ERR_INVALID_ARG;
    }
    const device_t *dev = &s_devices[slot];
    if (max_records < dev->count) {
        return EMS_ERR_NO_SPACE;
    }
    lock();
    for (uint8_t i = 0u; i < dev->count; ++i) {
        out[i] = dev->entries[i].rec;
    }
    unlock();
    *out_count = dev->count;
    return EMS_OK;
}
