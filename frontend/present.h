#ifndef FRONTEND_PRESENT_H
#define FRONTEND_PRESENT_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Frame presentation that never makes the emulation thread wait.
 *
 * On the R1 a framebuffer pan blocks until the LCD controller has taken the
 * frame (lcdc_pan_display in the soc_fb driver waits for the frame interrupt on
 * RGB panels, whatever the pan_display_sync parameter says, and the driver has
 * no FBIO_WAITFORVSYNC: any other ioctl "does nothing and returns 0"). Doing the
 * pan in the emulation thread costs up to a refresh per frame, which a core that
 * is already over budget cannot afford.
 *
 * So the emulation thread draws into a RAM buffer and hands it over; a
 * presenter thread copies the newest handed-over frame into the framebuffer
 * page that is not on screen and pans, which is the only place that waits.
 * Three RAM buffers rotate (being drawn, waiting, being shown), and the hand-over
 * is latest-wins: a frame still waiting when the next one arrives is dropped,
 * never queued. Frames that do reach the screen are never torn (the page being
 * written is not the one displayed).
 *
 * Everything else that writes the framebuffer (the touch controls, the pause
 * menu) goes through present_claim()/present_release(): the claim discards the
 * waiting frame, waits for a copy in progress to finish and keeps the presenter
 * out until it is released, so there are no races on the pages. Only the
 * emulation thread submits and claims. */

/* Shows rows rows of frame px (the buffer returned by present_buffer, stride as
 * given to present_start). Runs in the presenter thread, without the lock. */
typedef void (*present_show_fn)(void * ctx, const uint16_t * px, int rows);

typedef struct {
    uint16_t * buf[3];
    int draw, wait, show;            /* indexes: being drawn, waiting, being shown */
    int wait_rows;
    bool pending, busy, stop, started;
    pthread_mutex_t lock;
    pthread_cond_t work, idle;
    pthread_t thread;
    present_show_fn show_fn;
    void * ctx;
    /* statistics */
    uint64_t submitted, shown, dropped;
    uint64_t show_ns_total, show_ns_max;
} present_t;

/* buf_bytes: size of each of the three RAM buffers (zeroed). */
bool present_start(present_t * p, size_t buf_bytes, present_show_fn fn, void * ctx);

/* The buffer to draw the next frame into. */
uint16_t * present_buffer(const present_t * p);

/* Hands the drawn buffer over (latest wins) and returns at once; present_buffer
 * is a different buffer afterwards. */
void present_submit(present_t * p, int rows);

/* Exclusive use of the framebuffer: drops the waiting frame, waits for the
 * presenter to be idle and holds it off until present_release. */
void present_claim(present_t * p);
void present_release(present_t * p);

/* Shows the waiting frame (the last one is never lost), stops the thread and
 * frees the buffers. */
void present_stop(present_t * p);

#endif /* FRONTEND_PRESENT_H */
