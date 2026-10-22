#include "audio_policy.h"

#include <limits.h>

uint32_t audio_policy_period_frames(uint32_t rate, uint32_t duration_ms) {
    if (!duration_ms) return AUDIO_POLICY_SAFE_PERIOD_FRAMES;
    if (rate < 8000 || rate > 192000 || duration_ms > 10000)
        return AUDIO_POLICY_SAFE_PERIOD_FRAMES;
    /* Four periods total; use 64-bit arithmetic and round to nearest. */
    uint64_t numerator = (uint64_t) rate * duration_ms;
    uint64_t denominator = 1000u * AUDIO_POLICY_PERIOD_COUNT;
    uint64_t target = (numerator + denominator / 2) / denominator;
    if (target <= AUDIO_POLICY_MIN_PERIOD_FRAMES) return AUDIO_POLICY_MIN_PERIOD_FRAMES;
    if (target >= AUDIO_POLICY_MAX_PERIOD_FRAMES) return AUDIO_POLICY_MAX_PERIOD_FRAMES;
    uint32_t lower = AUDIO_POLICY_MIN_PERIOD_FRAMES;
    while ((uint64_t) lower * 2 < target) lower *= 2;
    uint32_t upper = lower * 2;
    return target - lower <= upper - target ? lower : upper;
}

uint32_t audio_policy_open_fallback(uint32_t requested, bool first_succeeded) {
    if (first_succeeded || requested == AUDIO_POLICY_SAFE_PERIOD_FRAMES)
        return requested;
    return AUDIO_POLICY_SAFE_PERIOD_FRAMES;
}

bool audio_policy_restore_safe(uint32_t active, unsigned underruns) {
    return active != AUDIO_POLICY_SAFE_PERIOD_FRAMES &&
           underruns >= AUDIO_POLICY_UNDERRUN_LIMIT;
}

bool audio_policy_capacity(uint32_t period, uint32_t count, uint32_t *capacity) {
    if (!period || period > AUDIO_POLICY_MAX_PERIOD_FRAMES || !count || !capacity ||
        period > UINT32_MAX / count / 4u)
        return false;
    *capacity = period * count;
    return true;
}

bool audio_policy_geometry_changed(uint32_t requested_period, uint32_t requested_count,
                                   uint32_t actual_period, uint32_t actual_count) {
    return requested_period != actual_period || requested_count != actual_count;
}
