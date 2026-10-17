#ifndef FRONTEND_STRETCH_H
#define FRONTEND_STRETCH_H

#include <stdbool.h>
#include <stdint.h>

/* Audio stretching for an emulator that cannot keep up with real time.
 *
 * The frontend paces the game on blocking audio writes, so when the core runs
 * slower than real time the device buffer drains and underruns. The stretch
 * controller watches how full that buffer is and lowers the resampling ratio
 * (below 1.0 the game plays slower and lower, with more output frames per
 * input frame) just enough that the buffer stops draining.
 *
 *   - Until the stream has started (the buffer filled once) and while the
 *     smoothed fill stays above STRETCH_LOW the ratio is exactly 1.0 and the
 *     audio path is the plain one. A core that keeps up leaves the buffer
 *     nearly full, because the blocking write waits for room.
 *   - Below it the target ratio is 1 - STRETCH_KP * (low mark - fill): the
 *     slower the core, the lower the fill settles under the low mark. A pure
 *     proportional law is used on purpose: the plant (the fill) is an
 *     integrator, an integral term on top of it limit-cycled in simulation
 *     (it overshoots, then unwinds into an underrun), and the standing offset
 *     of P is small and harmless (a 0.75x core settles at 45% fill).
 *   - The ratio follows the target through a slew limiter (fast enough to
 *     stop a draining buffer, slow when recovering) and is clamped to
 *     STRETCH_MIN_RATIO, so pitch changes stay gradual. A core slower than
 *     the minimum still underruns, as before.
 *
 * Pure logic, no device access: the frontend feeds it the device fill, the
 * host tests feed it a simulated device. */

#define STRETCH_MIN_RATIO 0.75   /* slowest the game is ever played */
#define STRETCH_LOW 0.70         /* fill (of the buffer) under which the ratio drops */
#define STRETCH_START 0.75       /* the stream counts as started once the fill has reached this */
#define STRETCH_KP 1.0           /* ratio lost per unit of fill below the low mark */
#define STRETCH_MAX_DOWN 0.5     /* ratio change per second when stretching more */
#define STRETCH_MAX_UP 0.2       /* ratio change per second when recovering */
#define STRETCH_FILL_TAU 0.07    /* seconds of the fill smoothing */

#define STRETCH_Q16_ONE 65536u   /* the ratio as handed out: 16.16 fixed point */

typedef struct {
    unsigned buffer_frames, rate;
    bool primed;
    double fill;                 /* smoothed fill, frames */
    double ratio;                /* current, slewed */
    /* statistics, weighted by the audio time they cover */
    double seconds, stretched_seconds, ratio_seconds, fill_seconds;
    double ratio_min;
} stretch_t;

typedef struct {
    double min_ratio, avg_ratio, stretched_percent, avg_fill_percent, seconds;
} stretch_stats_t;

void stretch_init(stretch_t * s, unsigned buffer_frames, unsigned rate);

/* Feeds one measurement: the frames in the device buffer after a write that
 * added out_frames of audio. Returns the ratio to use from now on in 16.16
 * (STRETCH_Q16_ONE is 1.0, and then the audio path must be the plain one). */
uint32_t stretch_update(stretch_t * s, unsigned fill, unsigned out_frames);

void stretch_stats(const stretch_t * s, stretch_stats_t * out);

/* Frames in a playback buffer of buffer_frames from the "available" count the
 * device reports (pcm_get_htimestamp: hw_ptr + buffer - appl_ptr, wrapped into
 * the ring's boundary, taken from the last successful SYNC_PTR ioctl). Valid
 * readings are 0..buffer_frames. A count slightly above the buffer size means
 * the hardware pointer has passed the application pointer (the buffer ran dry;
 * the kernel declares the xrun when it sees it, stop_threshold being the buffer
 * size): the fill is 0. A count beyond twice the buffer is not a measurement
 * (a stale or wrapped value): -1, and the controller must not be fed. */
int stretch_fill_from_avail(unsigned avail, unsigned buffer_frames);

/* Input frames per output frame of the resampler (32.32) for a ratio: the
 * base step divided by the ratio; exactly the base step at STRETCH_Q16_ONE. */
uint64_t stretch_step(uint64_t base_step, uint32_t ratio_q16);

#endif /* FRONTEND_STRETCH_H */
