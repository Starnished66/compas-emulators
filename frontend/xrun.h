#ifndef FRONTEND_XRUN_H
#define FRONTEND_XRUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Audio writes with real underrun accounting, independent of tinyalsa so the
 * host tests can drive it with a simulated device.
 *
 * tinyalsa (the pinned commit, pcm_generic_transfer in src/pcm.c) handles an
 * ALSA xrun by itself: the write ioctl fails with EPIPE, it increments a
 * private counter that has no accessor, calls pcm_prepare and writes again, so
 * the caller sees a successful write. The frontend therefore opens the device
 * with PCM_NORESTART: then the same code counts the xrun, skips the recovery
 * and returns -1 with errno still EPIPE, and this module does the recovery
 * (prepare, write again) and the counting. A write the kernel only partly
 * completed before an xrun returns the frames done; the rest is written
 * again, so no audio is lost to the recovery. */

typedef struct {
    /* Writes up to frames frames and returns how many went out, or a negative
     * value with errno set (EPIPE for an underrun, ESTRPIPE after a suspend). */
    int (*write)(void * ctx, const int16_t * data, unsigned frames);
    /* Re-prepares the device after an xrun; false on failure. */
    bool (*prepare)(void * ctx);
    void * ctx;
} xrun_device_t;

typedef struct {
    unsigned underruns;              /* writes that failed with EPIPE: real ALSA underruns */
    unsigned suspends;               /* writes that failed with ESTRPIPE */
    unsigned prepares;               /* recoveries by prepare, after either of the above */
    unsigned errors;                 /* other failures: the caller paces the frame itself */
} xrun_stats_t;

/* Writes every frame, recovering from underruns and suspends (at most
 * XRUN_MAX_RECOVERIES per call). False when a frame could not be written. */
#define XRUN_MAX_RECOVERIES 3
bool xrun_write_all(const xrun_device_t * dev, xrun_stats_t * stats, const int16_t * data, unsigned frames);

#endif /* FRONTEND_XRUN_H */
