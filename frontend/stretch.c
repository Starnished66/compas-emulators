#include "stretch.h"

#include <string.h>

void stretch_init(stretch_t * s, unsigned buffer_frames, unsigned rate) {
    memset(s, 0, sizeof(*s));
    s->buffer_frames = buffer_frames;
    s->rate = rate;
    s->ratio = 1.0;
    s->ratio_min = 1.0;
}

uint32_t stretch_update(stretch_t * s, unsigned fill, unsigned out_frames) {
    if (!s->buffer_frames || !s->rate) return STRETCH_Q16_ONE;
    const double dt = (double) out_frames / s->rate;
    if (fill > s->buffer_frames) fill = s->buffer_frames;
    if (!s->primed) {
        /* The buffer fills up from empty when the stream starts: that is not falling behind. */
        if (fill < STRETCH_START * s->buffer_frames) return STRETCH_Q16_ONE;
        s->primed = true;
        s->fill = fill;
    } else if (dt > 0) {
        s->fill += (fill - s->fill) * (dt / (STRETCH_FILL_TAU + dt));
    }
    if (dt > 0) {
        const double f = s->fill / s->buffer_frames;
        double target = f < STRETCH_LOW ? 1.0 - STRETCH_KP * (STRETCH_LOW - f) : 1.0;
        if (target < STRETCH_MIN_RATIO) target = STRETCH_MIN_RATIO;
        if (target < s->ratio) {
            s->ratio -= STRETCH_MAX_DOWN * dt;
            if (s->ratio < target) s->ratio = target;
        } else {
            s->ratio += STRETCH_MAX_UP * dt;
            if (s->ratio > target) s->ratio = target;
        }
        const uint32_t q = (uint32_t) (s->ratio * STRETCH_Q16_ONE + 0.5);
        if (q >= STRETCH_Q16_ONE) s->ratio = 1.0;
        if (s->ratio < s->ratio_min) s->ratio_min = s->ratio;
        s->seconds += dt;
        if (q < STRETCH_Q16_ONE) s->stretched_seconds += dt;
        s->ratio_seconds += s->ratio * dt;
        s->fill_seconds += ((double) fill / s->buffer_frames) * dt;
    }
    return (uint32_t) (s->ratio * STRETCH_Q16_ONE + 0.5);
}

void stretch_stats(const stretch_t * s, stretch_stats_t * out) {
    out->seconds = s->seconds;
    out->min_ratio = s->ratio_min;
    out->avg_ratio = s->seconds > 0 ? s->ratio_seconds / s->seconds : 1.0;
    out->stretched_percent = s->seconds > 0 ? 100.0 * s->stretched_seconds / s->seconds : 0.0;
    out->avg_fill_percent = s->seconds > 0 ? 100.0 * s->fill_seconds / s->seconds : 0.0;
}

uint64_t stretch_step(uint64_t base_step, uint32_t ratio_q16) {
    if (ratio_q16 == STRETCH_Q16_ONE || ratio_q16 == 0) return base_step;
    /* base_step is below 2^36 for any core rate under 8x the device rate: the product fits. */
    return (base_step * STRETCH_Q16_ONE + ratio_q16 / 2) / ratio_q16;
}

int stretch_fill_from_avail(unsigned avail, unsigned buffer_frames) {
    if (!buffer_frames) return -1;
    if (avail <= buffer_frames) return (int) (buffer_frames - avail);
    return avail <= 2u * buffer_frames ? 0 : -1;
}
