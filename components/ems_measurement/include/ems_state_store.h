/*
 * ems_state_store.h - MyEMS measurement state store (DP-WP4 4B, Design 6.4)
 *
 * A fixed table of ems_measurement_record_t, one per (device slot, parameter)
 * declared in the layout given to ems_state_init().
 *
 * Ownership
 *   - Writer API (publish / mark_* / set_device_* / check_freshness): the
 *     acquisition task only. Single writer.
 *   - Reader API (get / snapshot_device): any task. Copies out under the lock.
 *
 * Locking is injected (ems_state_lock_t) so that this file has no RTOS
 * dependency. On the target pass short critical-section wrappers; in host
 * tests pass NULL. The lock is held only while copying records - never across
 * a blocking call.
 *
 * Memory is static; sizes are EMS_MAX_DEVICES x EMS_MAX_PARAMS_PER_DEVICE
 * (override with compile definitions if needed).
 *
 * Quality rules (Design 6.2 / Figure 3):
 *   publish(GOOD)          -> GOOD, value and sample time updated
 *   publish(OUT_OF_RANGE)  -> OUT_OF_RANGE, value kept for diagnostics,
 *                             sample time unchanged (last GOOD sample)
 *   mark_failed            -> GOOD becomes STALE; other qualities unchanged
 *   mark_invalid           -> INVALID; value and sample time unchanged
 *   set_device_comm_lost   -> every parameter of the device COMM_LOST
 *   check_freshness        -> GOOD older than its stale timeout becomes STALE
 *   seq is incremented by every publish and by every quality change, so a
 *   reader can tell that a record was written even if the value is the same.
 */
#ifndef EMS_STATE_STORE_H
#define EMS_STATE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ems_measurement.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EMS_MAX_DEVICES
#define EMS_MAX_DEVICES 4u
#endif

#ifndef EMS_MAX_PARAMS_PER_DEVICE
#define EMS_MAX_PARAMS_PER_DEVICE 32u
#endif

/* One parameter a device provides, with the stale timeout of its poll group. */
typedef struct {
    uint16_t param_id;
    uint32_t stale_timeout_ms;   /* > 0 */
} ems_state_param_def_t;

/* One device slot. Slot number = index in ems_state_layout_t.devices. */
typedef struct {
    const ems_state_param_def_t *params;
    uint8_t param_count;         /* <= EMS_MAX_PARAMS_PER_DEVICE */
    bool    enabled;             /* false: every parameter NOT_SUPPORTED */
} ems_state_device_def_t;

typedef struct {
    const ems_state_device_def_t *devices;
    uint8_t device_count;        /* 1 .. EMS_MAX_DEVICES */
} ems_state_layout_t;

typedef void (*ems_lock_fn_t)(void *ctx);

typedef struct {
    ems_lock_fn_t lock;
    ems_lock_fn_t unlock;
    void *ctx;
} ems_state_lock_t;

/*
 * Validate the layout and initialise every declared parameter to
 * INITIALIZING (NOT_SUPPORTED for disabled devices). The layout is copied.
 * lock may be NULL (no locking, host tests). Re-initialisation is allowed only
 * after ems_state_deinit().
 * Returns EMS_ERR_INVALID_ARG (NULL/empty, zero timeout, duplicate parameter),
 * EMS_ERR_NOT_FOUND (parameter not in dictionary), EMS_ERR_NO_SPACE (limits),
 * EMS_ERR_INVALID_STATE (already initialised).
 */
ems_status_t ems_state_init(const ems_state_layout_t *layout, const ems_state_lock_t *lock);

/* Forget everything; afterwards ems_state_init() may be called again. */
void ems_state_deinit(void);

bool ems_state_is_initialised(void);

/* ---- Writer API: acquisition task only ---------------------------------- */

/*
 * Store a decoded value. quality must be EMS_Q_GOOD or EMS_Q_OUT_OF_RANGE and
 * value must be finite (a non-finite result is a decode failure: use
 * ems_state_mark_invalid instead).
 */
ems_status_t ems_state_publish(uint8_t slot, uint16_t param_id, double value,
                               ems_quality_t quality, int64_t now_mono_us);

/* A read of this parameter's poll group failed (timeout, CRC, ...). */
ems_status_t ems_state_mark_failed(uint8_t slot, uint16_t param_id);

/* Decode failed or the device answered with a Modbus exception. */
ems_status_t ems_state_mark_invalid(uint8_t slot, uint16_t param_id);

/* The device went OFFLINE: all its parameters become COMM_LOST. */
ems_status_t ems_state_set_device_comm_lost(uint8_t slot);

/*
 * Turn every GOOD parameter whose last sample is older than its stale
 * timeout into STALE. Age exactly equal to the timeout is still GOOD.
 * Returns the number of parameters changed.
 */
uint32_t ems_state_check_freshness(int64_t now_mono_us);

/* ---- Reader API: any task ----------------------------------------------- */

/*
 * Copy one record. For a parameter that exists in the dictionary but is not
 * declared for this device, returns EMS_OK with quality NOT_SUPPORTED and
 * seq 0. EMS_ERR_NOT_FOUND if the parameter is not in the dictionary,
 * EMS_ERR_INVALID_ARG for a bad slot or NULL out.
 */
ems_status_t ems_state_get(uint8_t slot, uint16_t param_id, ems_measurement_record_t *out);

/* Copy all declared records of one device, in layout order. */
ems_status_t ems_state_snapshot_device(uint8_t slot, ems_measurement_record_t *out,
                                       size_t max_records, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* EMS_STATE_STORE_H */
