#ifndef FRONTEND_AUDIO_POLICY_H
#define FRONTEND_AUDIO_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_POLICY_SAFE_PERIOD_FRAMES 1024u
#define AUDIO_POLICY_PERIOD_COUNT 4u
#define AUDIO_POLICY_MIN_PERIOD_FRAMES 256u
#define AUDIO_POLICY_MAX_PERIOD_FRAMES 4096u
#define AUDIO_POLICY_UNDERRUN_LIMIT 3u

/* duration_ms==0 selects the proven safe default. Invalid inputs also fall
 * back to that default. Otherwise chooses the nearest legal power of two. */
uint32_t audio_policy_period_frames(uint32_t rate, uint32_t duration_ms);
/* A failed short-period open gets one safe-size retry. */
uint32_t audio_policy_open_fallback(uint32_t requested_period, bool first_open_succeeded);
/* Once short mode reaches the threshold, restore the safe period. */
bool audio_policy_restore_safe(uint32_t active_period, unsigned real_underruns);

/* Validate driver geometry before multiplying it or using it as a threshold.
 * The PCM API expresses capacity in unsigned frames; leave byte conversion
 * (stereo S16) representable as well. */
bool audio_policy_capacity(uint32_t period, uint32_t count, uint32_t *capacity);
/* One correction open is allowed when the driver changes the request. */
bool audio_policy_geometry_changed(uint32_t requested_period, uint32_t requested_count,
                                   uint32_t actual_period, uint32_t actual_count);

#endif
