/*
 * ems_counter.c - cumulative energy counter tracking (DP-WP4 4C, Design 8.5)
 */
#include "ems_counter.h"

#include <stddef.h>
#include <string.h>

void ems_counter_init(ems_counter_t *c, bool rollover_enabled, double unit_per_count)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->rollover_enabled = rollover_enabled;
    c->unit_per_count = unit_per_count;
}

static void set_baseline(ems_counter_t *c, uint32_t raw, double value, int64_t now_mono_us)
{
    c->has_baseline = true;
    c->last_raw = raw;
    c->last_value = value;
    c->last_mono_us = now_mono_us;
}

ems_counter_event_t ems_counter_update(ems_counter_t *c, uint32_t raw, double value,
                                       int64_t now_mono_us, double max_rate_per_hour,
                                       double *out_delta)
{
    double delta = 0.0;
    ems_counter_event_t ev;

    if (c == NULL) {
        if (out_delta != NULL) {
            *out_delta = 0.0;
        }
        return EMS_CNT_DISCONTINUITY;
    }

    if (!c->has_baseline) {
        ev = EMS_CNT_FIRST;
    } else if (raw >= c->last_raw) {
        /* Normal increase or unchanged: use engineering values (exact decimals). */
        delta = value - c->last_value;
        ev = EMS_CNT_DELTA;
    } else if (c->rollover_enabled) {
        /* Unsigned subtraction gives the count difference modulo 2^32. */
        const uint32_t counts = raw - c->last_raw;
        const double wrap_delta = (double)counts * c->unit_per_count;
        bool plausible = true;
        if (max_rate_per_hour > 0.0) {
            const int64_t elapsed_us = now_mono_us - c->last_mono_us;
            const double elapsed_h = (elapsed_us > 0) ? (double)elapsed_us / 3.6e9 : 0.0;
            plausible = wrap_delta <= 2.0 * max_rate_per_hour * elapsed_h;
        }
        if (plausible) {
            delta = wrap_delta;
            ev = EMS_CNT_ROLLOVER;
        } else {
            ev = EMS_CNT_DISCONTINUITY;
        }
    } else {
        ev = EMS_CNT_DISCONTINUITY;
    }

    set_baseline(c, raw, value, now_mono_us);
    c->last_delta = delta;
    if (out_delta != NULL) {
        *out_delta = delta;
    }
    return ev;
}
