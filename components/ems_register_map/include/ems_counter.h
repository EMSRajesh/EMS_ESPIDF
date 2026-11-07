/*
 * ems_counter.h - cumulative energy counter tracking (DP-WP4 4C, Design 8.5)
 *
 * One tracker per counter parameter (e.g. ac.energy.import.active). Call
 * ems_counter_update() only after a GOOD read of the counter's poll group; a
 * failed read must not call it (ENG-007: baseline and timestamp unchanged).
 *
 *   new >= old                         -> EMS_CNT_DELTA, delta = new - old
 *   new <  old, rollover enabled and
 *        wrap delta is plausible       -> EMS_CNT_ROLLOVER, delta across 2^32
 *   new <  old otherwise               -> EMS_CNT_DISCONTINUITY, no delta,
 *                                         new baseline (reset or meter swap)
 *   first sample                       -> EMS_CNT_FIRST, no delta
 *
 * The raw counter value is always the truth: it is published as GOOD in every
 * case; only the delta is withheld on a discontinuity.
 *
 * Pure C, no dynamic memory.
 */
#ifndef EMS_COUNTER_H
#define EMS_COUNTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EMS_CNT_FIRST = 0,       /* baseline set, no delta                 */
    EMS_CNT_DELTA,           /* normal increase (or unchanged), delta >= 0 */
    EMS_CNT_ROLLOVER,        /* wrapped at 2^32, delta computed across wrap */
    EMS_CNT_DISCONTINUITY    /* decrease/reset, no delta, new baseline */
} ems_counter_event_t;

typedef struct {
    bool     has_baseline;
    bool     rollover_enabled;
    uint32_t last_raw;
    double   last_value;
    int64_t  last_mono_us;
    double   unit_per_count;   /* engineering units per raw count (scale x ratio) */
    double   last_delta;       /* delta of the last update, 0 if none     */
} ems_counter_t;

/*
 * unit_per_count: engineering value of one raw count after scale and ratio,
 * e.g. 0.01 kWh x CT 200 = 2.0. Used only for the rollover delta.
 */
void ems_counter_init(ems_counter_t *c, bool rollover_enabled, double unit_per_count);

/*
 * raw, value: decoded raw counter and its engineering value (after scale and
 * ratio) from the same GOOD read.
 * max_rate_per_hour: plausibility limit in units per hour (for an energy
 * counter in kWh: CT x PT x maximum active power in kW). The largest accepted
 * rollover delta is 2 x max_rate_per_hour x elapsed hours. Pass 0 or less to
 * accept any rollover delta.
 * out_delta (may be NULL) receives the delta, or 0 when there is none.
 */
ems_counter_event_t ems_counter_update(ems_counter_t *c, uint32_t raw, double value,
                                       int64_t now_mono_us, double max_rate_per_hour,
                                       double *out_delta);

#ifdef __cplusplus
}
#endif

#endif /* EMS_COUNTER_H */
