/* Shared libretro frontend for Compas emulator packages (protocol 1, see
 * docs/PROTOCOL.md). One copy serves every emulator: the core is linked
 * statically and EMULATOR_ID names the program in its log lines.
 *
 * It maps the framebuffer, grabs every input device so the player underneath
 * sees no events, draws the game at the top of the screen with touch
 * controls below it (a presenter thread owns the framebuffer pan, see
 * present.h), and paces itself on blocking audio writes. A tap in the
 * middle of the game or a short power button press opens the pause menu
 * (resume, save state, load state, reset, exit); a one second power button
 * hold or SIGTERM exits from anywhere. Per-emulator settings (which buttons
 * to draw, core option answers) come from the "frontend" object of the
 * emulator's own emulator.json, see config.h. */
#include "frontend.h"

#include "config.h"
#include "glyph5x7.h"
#include "present.h"
#include "video_scale.h"
#include "residency.h"
#include "audio_policy.h"
#include "stretch.h"
#include "xrun.h"
#include "libretro.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef HOST_BUILD
#include <tinyalsa/asoundlib.h>
#endif

#ifndef EMULATOR_ID
#define EMULATOR_ID "emulator"
#endif
#define LOG_PREFIX EMULATOR_ID ": "
#define MAX_SURFACE_WIDTH 1024
#define MAX_SURFACE_HEIGHT 2048
#define MAX_INPUT_DEVICES 8
#define MAX_CONTACTS 10
#define POWER_HOLD_QUIT_NS 1000000000ull
#define RELEASE_WAIT_NS 2000000000ull
#define WATCHDOG_STALL_SECONDS 5
#define TOUCH_TRACE_LINES 40
#define TAP_MAX_NS 300000000ull
#define TAP_MAX_MOVE_PX 24

static volatile sig_atomic_t quit_flag;
static atomic_uint heartbeat; /* advanced by every pass of the main loop */
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static char save_dir[512] = ".";
static char emu_dir[512] = ".";        /* the emulator's own folder: system directory and emulator.json */
static frontend_config_t cfg;
static bool variables_dirty;           /* answer GET_VARIABLE_UPDATE once after loading */
static bool video_skip;                /* the video of the frame being run is not wanted (frameskip) */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static double cpu_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static void runner_log(const char * fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, LOG_PREFIX "%s\n", line);
}

/* ---- output surface ---- */

static uint16_t * surf;              /* where the next frame is scaled to */
static int surf_w, surf_h, surf_stride; /* stride in pixels */
static int max_game_h;               /* tallest game area allowed */
static int game_h;                   /* scaled game height of the current frame */
static int draw_w, draw_x0;          /* scaled width and left margin (narrowed pictures are centered) */
static video_scale_t main_scale;
static uint64_t presenter_scale_ns;
static pthread_mutex_t video_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static int map_src_w, map_src_h;
static unsigned last_src_w, last_src_h;
static uint64_t video_frames;        /* frames scaled and shown */
static uint64_t video_skipped;       /* frames whose video was skipped on request */
static uint64_t scale_ns_total;
static uint16_t * shadow;            /* scaled copy of the last game frame, for the pause menu */
static int shadow_rows;              /* rows of it that hold a frame */
static bool capture_frame;           /* video_cb also stores the frame in shadow */

static void relayout_for_game(void);
static void fb_claim(void);
static void fb_release(void);
static bool present_frame(const void * data, unsigned w, unsigned h, size_t pitch);
static bool presenting;

/* Width-fit scaling: the picture fills the screen width unless that would
 * make it taller than max_game_h, then it is narrowed to keep its shape. */
static void build_maps(int src_w, int src_h) {
    if (!video_scale_init(&main_scale, src_w, src_h, surf_w, max_game_h)) {
        runner_log("invalid video geometry %dx%d", src_w, src_h);
        quit_flag = 1;
        return;
    }
    map_src_w = src_w;
    map_src_h = src_h;
    draw_w = main_scale.draw_w;
    draw_x0 = main_scale.draw_x0;
    game_h = main_scale.game_h;
}

static video_scale_format_t scale_format(enum retro_pixel_format format) {
    if (format == RETRO_PIXEL_FORMAT_RGB565) return VIDEO_SCALE_RGB565;
    if (format == RETRO_PIXEL_FORMAT_XRGB8888) return VIDEO_SCALE_XRGB8888;
    return VIDEO_SCALE_RGB1555;
}

/* Benchmarks and pause snapshots scale synchronously. Interactive frames are
 * copied at native resolution; only the newest pending frame is upscaled by
 * the presenter. A slow display cannot make the core scale discarded frames. */
static void video_cb(const void * data, unsigned w, unsigned h, size_t pitch) {
    if (!data || !w || !h || (!presenting && !surf && !capture_frame)) return;
    last_src_w = w;
    last_src_h = h;
    if (video_skip) { video_skipped++; return; }
    if (w > MAX_SURFACE_WIDTH || h > MAX_SURFACE_HEIGHT) {
        runner_log("unsupported native frame %ux%u", w, h);
        quit_flag = 1;
        return;
    }
    if ((int) w != map_src_w || (int) h != map_src_h) {
        int old_game_h = game_h;
        build_maps((int) w, (int) h);
        if (quit_flag) return;
        if (game_h != old_game_h) relayout_for_game();
    }
    if (presenting && !capture_frame) {
        if (!present_frame(data, w, h, pitch)) quit_flag = 1;
    } else {
        uint16_t * dst = capture_frame ? shadow : surf;
        size_t stride = capture_frame ? (size_t) surf_w : (size_t) surf_stride;
        uint64_t t0 = now_ns();
        if (!video_scale_render(&main_scale, data, pitch, scale_format(pixfmt), dst, stride)) {
            runner_log("invalid video frame pitch");
            quit_flag = 1;
            return;
        }
        scale_ns_total += now_ns() - t0;
        if (capture_frame) shadow_rows = game_h;
    }
    video_frames++;
}

/* The MIPS target has no lock-free 64-bit atomics. Only interval reporting
 * takes this lock from the core thread; normal frames never wait for it. */
static uint64_t video_scaling_ns(void) {
    pthread_mutex_lock(&video_stats_lock);
    uint64_t total = scale_ns_total + presenter_scale_ns;
    pthread_mutex_unlock(&video_stats_lock);
    return total;
}

/* ---- audio ---- */

static int16_t audio_buf[8192 * 2];
static size_t audio_buf_frames;
static uint64_t audio_frames_total;

static size_t audio_batch_cb(const int16_t * data, size_t frames) {
    size_t room = sizeof(audio_buf) / sizeof(audio_buf[0]) / 2 - audio_buf_frames;
    size_t n = frames < room ? frames : room;
    memcpy(audio_buf + audio_buf_frames * 2, data, n * 4);
    audio_buf_frames += n;
    audio_frames_total += frames;
    return frames;
}

static void audio_sample_cb(int16_t l, int16_t r) {
    int16_t s[2] = { l, r };
    audio_batch_cb(s, 1);
}

#ifndef HOST_BUILD
/* The R1 codec accepts four periods. 46 ms is the requested default latency; three real
 * EPIPE recoveries restore the proven 1024-frame safe period for this run. */
/* The player's audio thread closes its device asynchronously after a pause. */
#define AUDIO_OPEN_RETRY_MS 100
#define AUDIO_OPEN_TIMEOUT_MS 2000

#ifdef FRONTEND_TEST_PCM_AVAIL_MIN_FRAMES
_Static_assert(FRONTEND_TEST_PCM_AVAIL_MIN_FRAMES > 0,
               "FRONTEND_TEST_PCM_AVAIL_MIN_FRAMES must be positive");
#endif

static struct pcm * pcm;
static unsigned audio_active_period = AUDIO_POLICY_SAFE_PERIOD_FRAMES;
static unsigned audio_short_underrun_base;
static bool audio_forced_safe;
static xrun_stats_t audio_xr;        /* real underruns and recoveries, see xrun.h */
static stretch_t stretch;
static uint32_t stretch_ratio_q16 = STRETCH_Q16_ONE;
static double stretch_next_log;      /* audio seconds of the next periodic log line */
#endif

/* The codec only has the usual rates. A core with another one (gpSP renders
 * at 32768 or 65536 Hz) is resampled to 48000 Hz with linear interpolation. */
static unsigned device_rate_for(unsigned rate) {
    static const unsigned standard[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000 };
    for (size_t i = 0; i < sizeof(standard) / sizeof(standard[0]); i++)
        if (rate == standard[i]) return rate;
    return 48000;
}

static unsigned core_rate, device_rate;
/* Read position in input frames, 32.32 fixed point, counted from the carried
 * frame: 0 is the frame kept from the previous block, 1.0 is the first frame
 * of this block. Integer only, so the cost does not depend on an FPU. */
static uint64_t resample_pos;
static uint64_t resample_step;       /* input frames per output frame, 32.32 */
static int16_t resample_prev[2];     /* last input frame of the previous block */
static int16_t * resample_buf;
static size_t resample_cap;
static uint64_t resample_base_step;  /* resample_step at ratio 1.0 */
/* Audio stretching (cfg.audio_stretch); the controller state is with the device below. */
static bool stretch_on;

/* Converts audio_buf to the device rate with linear interpolation; returns
 * the frames in resample_buf. */
static size_t resample_audio(void) {
    const size_t n = audio_buf_frames;
    const uint64_t end = (uint64_t) n << 32;
    size_t out = 0;
    while (resample_pos < end && out < resample_cap) {
        const size_t k = (size_t) (resample_pos >> 32);
        const int32_t f = (int32_t) ((resample_pos >> 17) & 0x7FFF); /* 15 bits: (b - a) * f fits in 32 */
        for (size_t ch = 0; ch < 2; ch++) {
            const int32_t a = k ? audio_buf[(k - 1) * 2 + ch] : resample_prev[ch];
            const int32_t b = audio_buf[k * 2 + ch];
            resample_buf[out * 2 + ch] = (int16_t) (a + (((b - a) * f) >> 15));
        }
        out++;
        resample_pos += resample_step;
    }
    if (n) {
        resample_prev[0] = audio_buf[(n - 1) * 2];
        resample_prev[1] = audio_buf[(n - 1) * 2 + 1];
        resample_pos = resample_pos > end ? resample_pos - end : 0;
    }
    return out;
}

static bool resample_init(unsigned rate) {
    core_rate = rate;
    device_rate = device_rate_for(rate);
    resample_pos = 0;
    resample_base_step = (((uint64_t) core_rate << 32) + device_rate / 2) / device_rate;
    resample_step = resample_base_step;
    memset(resample_prev, 0, sizeof(resample_prev));
    free(resample_buf);
    resample_buf = NULL;
    resample_cap = 0;
    if (device_rate == core_rate && !stretch_on) return true;
    resample_cap = (size_t) ((double) (sizeof(audio_buf) / sizeof(audio_buf[0]) / 2) * device_rate / core_rate /
                             (stretch_on ? STRETCH_MIN_RATIO : 1.0)) + 8;
    resample_buf = malloc(resample_cap * 4);
    if (!resample_buf) {
        runner_log("audio: out of memory (continuing silent)");
        return false;
    }
    if (device_rate != core_rate) runner_log("audio: core rate %u Hz resampled to %u Hz", core_rate, device_rate);
    return true;
}

#ifndef HOST_BUILD
static bool audio_open_device_period(unsigned period, bool retry_busy) {
    struct pcm_config pcfg = { 0 };
    pcfg.channels = 2;
    pcfg.rate = device_rate;
    pcfg.period_size = period;
    pcfg.period_count = AUDIO_POLICY_PERIOD_COUNT;
    pcfg.format = PCM_FORMAT_S16_LE;
    uint32_t capacity;
    if (!audio_policy_capacity(pcfg.period_size, pcfg.period_count, &capacity))
        return false;
#ifdef FRONTEND_TEST_PCM_AVAIL_MIN_FRAMES
    pcfg.avail_min = FRONTEND_TEST_PCM_AVAIL_MIN_FRAMES;
#endif
    pcfg.start_threshold = capacity;
    pcfg.stop_threshold = capacity;
    bool corrected = false;
    for (int waited_ms = 0;;) {
        /* PCM_NORESTART makes underruns visible instead of silently recovering. */
        pcm = pcm_open(0, 0, PCM_OUT | PCM_NORESTART, &pcfg);
        if (pcm && pcm_is_ready(pcm)) {
            const struct pcm_config * active = pcm_get_config(pcm);
            if (!active || !audio_policy_capacity(active->period_size, active->period_count, &capacity)) {
                runner_log("audio: invalid negotiated PCM geometry");
                pcm_close(pcm);
                pcm = NULL;
                return false;
            }
            if (audio_policy_geometry_changed(pcfg.period_size, pcfg.period_count,
                                               active->period_size, active->period_count)) {
                if (corrected) {
                    runner_log("audio: PCM geometry changed again after correction; refusing inconsistent buffer");
                    pcm_close(pcm);
                    pcm = NULL;
                    return false;
                }
                runner_log("audio: PCM requested %u x %u, negotiated %u x %u; reopening before playback",
                           pcfg.period_size, pcfg.period_count, active->period_size, active->period_count);
                pcfg.period_size = active->period_size;
                pcfg.period_count = active->period_count;
                pcfg.start_threshold = capacity;
                pcfg.stop_threshold = capacity;
                /* Keep zero avail_min: tinyalsa resolves it to the new period.
                 * An explicit diagnostic override is intentionally preserved. */
                pcm_close(pcm);
                pcm = NULL;
                corrected = true;
                continue;
            }
            unsigned long expected_avail = pcfg.avail_min ? pcfg.avail_min : active->period_size;
            if (pcm_get_buffer_size(pcm) != capacity ||
                active->start_threshold != capacity || active->stop_threshold != capacity ||
                active->avail_min != expected_avail || active->avail_min > capacity) {
                runner_log("audio: inconsistent negotiated PCM buffer or thresholds");
                pcm_close(pcm);
                pcm = NULL;
                return false;
            }
            runner_log("audio: PCM %u Hz, requested period %u x %u, active %u x %u, buffer %u frames, start %lu stop %lu avail_min %lu",
                       pcfg.rate, period, AUDIO_POLICY_PERIOD_COUNT,
                       active->period_size, active->period_count, capacity,
                       active->start_threshold, active->stop_threshold, active->avail_min);
            stretch_init(&stretch, capacity, device_rate);
            stretch_ratio_q16 = STRETCH_Q16_ONE;
            stretch_next_log = 10.0;
            audio_active_period = active->period_size;
            audio_short_underrun_base = audio_xr.underruns;
            return true;
        }
        const char * why = pcm ? pcm_get_error(pcm) : "open failed";
        if (!retry_busy || waited_ms >= AUDIO_OPEN_TIMEOUT_MS) {
            runner_log(retry_busy ? "audio: %s (continuing silent)" : "audio: %s", why);
            if (pcm) pcm_close(pcm);
            pcm = NULL;
            return false;
        }
        if (pcm) pcm_close(pcm);
        pcm = NULL;
        usleep(AUDIO_OPEN_RETRY_MS * 1000);
        waited_ms += AUDIO_OPEN_RETRY_MS;
    }
}

static bool audio_open(unsigned rate) {
    stretch_on = cfg.audio_stretch;
    if (!resample_init(rate)) return false;
    unsigned period = audio_forced_safe ? AUDIO_POLICY_SAFE_PERIOD_FRAMES :
                      audio_policy_period_frames(device_rate, cfg.audio_buffer_ms);
    bool short_request = period != AUDIO_POLICY_SAFE_PERIOD_FRAMES;
    if (audio_open_device_period(period, false)) return true;
    if (short_request) {
        runner_log("audio: short period %u failed; retrying safe %u-frame periods", period,
                   AUDIO_POLICY_SAFE_PERIOD_FRAMES);
        period = audio_policy_open_fallback(period, false);
    }
    return audio_open_device_period(period, true);
}

static void audio_restore_safe_period(void) {
    if (audio_forced_safe || audio_active_period == AUDIO_POLICY_SAFE_PERIOD_FRAMES) return;
    audio_forced_safe = true;
    runner_log("audio: %u underruns with %u-frame periods; restoring safe %u-frame periods for this session",
               AUDIO_POLICY_UNDERRUN_LIMIT, audio_active_period, AUDIO_POLICY_SAFE_PERIOD_FRAMES);
    if (pcm) { pcm_close(pcm); pcm = NULL; }
    (void) audio_open_device_period(AUDIO_POLICY_SAFE_PERIOD_FRAMES, true);
}

/* pcm_state is not in tinyalsa's public header but is a plain exported function of pcm.c. */
int pcm_state(struct pcm * pcm);

/* Frames in the device buffer for the stretch controller, or -1 when there is
 * no valid reading. pcm_get_htimestamp reports "avail" = hw_ptr + buffer_size -
 * appl_ptr (pcm_mmap_playback_avail in src/pcm.c) from the last successful
 * SNDRV_PCM_IOCTL_SYNC_PTR; on this kernel (MIPS, no coherent mmap of the
 * status and control pages, sound/core/pcm_native.c) tinyalsa always uses that
 * ioctl. It keeps the previous values without any error when the sync fails,
 * which happens in the XRUN state (the hwsync returns -EPIPE) and while the
 * stream is prepared but not started, so a reading is only trusted while the
 * state is RUNNING. A count above the buffer size is the hardware pointer
 * having passed the application pointer (see stretch_fill_from_avail). */
static int device_fill(void) {
    unsigned avail;
    struct timespec ts;
    if (pcm_get_htimestamp(pcm, &avail, &ts) < 0) return -1;
    if (pcm_state(pcm) != PCM_STATE_RUNNING) return -1;
    return stretch_fill_from_avail(avail, pcm_get_buffer_size(pcm));
}

static int pcm_write_op(void * ctx, const int16_t * data, unsigned frames) {
    return pcm_writei(ctx, data, frames);
}

static bool pcm_prepare_op(void * ctx) {
    return pcm_prepare(ctx) == 0;
}

/* Writes frames to the device, counting and recovering from underruns. */
static bool pcm_write_counted(const int16_t * data, unsigned frames) {
    const xrun_device_t dev = { pcm_write_op, pcm_prepare_op, pcm };
    bool ok = xrun_write_all(&dev, &audio_xr, data, frames);
    if (audio_active_period != AUDIO_POLICY_SAFE_PERIOD_FRAMES &&
        audio_policy_restore_safe(audio_active_period, audio_xr.underruns - audio_short_underrun_base))
        audio_restore_safe_period();
    return ok;
}

/* False when the write failed, so the caller paces the frame itself. */
static bool audio_flush(void) {
    bool ok = pcm != NULL;
    const int16_t * out = audio_buf;
    size_t frames = audio_buf_frames;
    if (audio_buf_frames) {
        if (device_rate != core_rate || stretch_ratio_q16 != STRETCH_Q16_ONE) {
            resample_step = stretch_step(resample_base_step, stretch_ratio_q16);
            frames = resample_audio();
            out = resample_buf;
        } else {
            /* Plain path; keep the resampler ready (one frame behind the input, as it runs). */
            resample_prev[0] = audio_buf[(audio_buf_frames - 1) * 2];
            resample_prev[1] = audio_buf[(audio_buf_frames - 1) * 2 + 1];
            resample_pos = 1ull << 32;
        }
    }
    if (pcm && frames) ok = pcm_write_counted(out, (unsigned) frames);
    if (ok && pcm && frames && stretch_on) {
        const int fill = device_fill();
        if (fill >= 0) {
            stretch_ratio_q16 = stretch_update(&stretch, (unsigned) fill, (unsigned) frames);
            if (stretch.seconds >= stretch_next_log) {
                stretch_next_log += 10.0;
                if (stretch_ratio_q16 != STRETCH_Q16_ONE)
                    runner_log("audio stretch: ratio %.3f, device fill %d of %u frames, underruns %u",
                               (double) stretch_ratio_q16 / STRETCH_Q16_ONE, fill, stretch.buffer_frames,
                               audio_xr.underruns);
            }
        }
    }
    audio_buf_frames = 0;
    return ok;
}

/* One period of silence while the game is paused: keeps the device fed (no
 * underrun, no repeated last buffer) and paces the pause loop like real audio
 * does. False when there is no working device and the caller must sleep. */
static bool audio_silence(void) {
    static const int16_t quiet[AUDIO_POLICY_MAX_PERIOD_FRAMES * 2];
    if (!pcm) return false;
    return pcm_write_counted(quiet, audio_active_period);
}

static void audio_close(void) {
    if (pcm) pcm_close(pcm);
    pcm = NULL;
    runner_log("audio: real underruns %u (ALSA EPIPE on write), suspends %u, prepare recoveries %u, write errors %u",
               audio_xr.underruns, audio_xr.suspends, audio_xr.prepares, audio_xr.errors);
    if (stretch_on && stretch.seconds > 0) {
        stretch_stats_t st;
        stretch_stats(&stretch, &st);
        runner_log("audio stretch: ratio min %.3f avg %.3f, stretched %.1f%% of %.0fs, device fill avg %.0f%% "
                   "(readings while the stream was running)", st.min_ratio, st.avg_ratio, st.stretched_percent,
                   st.seconds, st.avg_fill_percent);
    }
}
#else
static bool audio_open(unsigned rate) { (void) resample_init(rate); return false; }
static bool audio_flush(void) {
    if (device_rate != core_rate && audio_buf_frames) resample_audio();
    audio_buf_frames = 0;
    return false;
}
static bool audio_silence(void) { return false; }
static void audio_close(void) {}
#endif

static unsigned audio_underrun_count(void) {
#ifndef HOST_BUILD
    return audio_xr.underruns;
#else
    return 0;
#endif
}

/* ---- framebuffer ---- */

static int fb_fd = -1;
static uint8_t * fb_mem;
static size_t fb_len;
static struct fb_var_screeninfo vinfo, vinfo_orig;
static int fb_page;                  /* page the presenter writes next (the one not on screen) */
static int fb_pages;
static present_t presenter;
/* Native-resolution queue; the presenter owns its scale plan and scratch. */
typedef struct {
    unsigned w, h;
    enum retro_pixel_format format;
} native_frame_t;
static size_t native_capacity;
static struct {
    uint64_t submitted, shown, dropped, show_ns_total, show_ns_max;
} retired_presenters;

static void retire_presenter_stats(void) {
    retired_presenters.submitted += presenter.submitted;
    retired_presenters.shown += presenter.shown;
    retired_presenters.dropped += presenter.dropped;
    retired_presenters.show_ns_total += presenter.show_ns_total;
    if (presenter.show_ns_max > retired_presenters.show_ns_max)
        retired_presenters.show_ns_max = presenter.show_ns_max;
}
static uint16_t * display_pixels;
static video_scale_t display_scale;
static uint64_t pan_ns_total, pan_ns_max, copy_ns_total; /* written by the presenter thread, read after it stopped */

static uint16_t * fb_page_ptr(int page) {
    return (uint16_t *) (fb_mem + (size_t) page * vinfo.yres * surf_stride * 2);
}

static bool fb_open(void) {
    struct fb_fix_screeninfo finfo;
    fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) || ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo)) {
        runner_log("fb0: %s", strerror(errno));
        return false;
    }
    if (vinfo.bits_per_pixel != 16 || vinfo.xres > MAX_SURFACE_WIDTH || vinfo.yres > MAX_SURFACE_HEIGHT) {
        runner_log("fb0: unsupported mode %ux%u at %u bpp", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
        return false;
    }
    vinfo_orig = vinfo;
    fb_len = finfo.smem_len;
    fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        runner_log("mmap fb0: %s", strerror(errno));
        fb_mem = NULL;
        return false;
    }
    surf_w = (int) vinfo.xres;
    surf_h = (int) vinfo.yres;
    surf_stride = (int) (finfo.line_length / 2);
    fb_pages = (int) (vinfo.yres_virtual / vinfo.yres);
    if (fb_pages < 1) fb_pages = 1;
    if (fb_pages > 2) fb_pages = 2;
    fb_page = (fb_pages > 1 && vinfo.yoffset == 0) ? 1 : 0;
    return true;
}

/* Presenter thread: copies a finished frame into the page that is not on screen
 * and pans to it. The pan is where the wait for the display happens (see
 * present.h). With a single page the copy goes straight to the screen. */
static void fb_show(void * ctx, const uint16_t * px, int rows) {
    (void) ctx;
    uint64_t t0 = now_ns();
    memcpy(fb_page_ptr(fb_page), px, ((size_t) (rows - 1) * (size_t) surf_stride + (size_t) surf_w) * 2);
    uint64_t t1 = now_ns();
    copy_ns_total += t1 - t0;
    if (fb_pages < 2) return;
    vinfo.yoffset = (unsigned) (fb_page * vinfo.yres);
    ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
    uint64_t dt = now_ns() - t1;
    pan_ns_total += dt;
    if (dt > pan_ns_max) pan_ns_max = dt;
    fb_page ^= 1;
}

/* The callback owns the scale plan and scratch; geometry/layout on the core
 * thread is independent. present_claim() serializes all framebuffer access. */
static void show_native_frame(void * ctx, const uint16_t * pixels, int rows) {
    (void) ctx;
    (void) rows;
    native_frame_t frame;
    memcpy(&frame, pixels, sizeof(frame));
    const uint8_t * src = (const uint8_t *) pixels + sizeof(frame);
    size_t pitch = (size_t) frame.w * (frame.format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4u : 2u);
    if (display_scale.src_w != (int) frame.w || display_scale.src_h != (int) frame.h) {
        if (!video_scale_init(&display_scale, (int) frame.w, (int) frame.h, surf_w, max_game_h)) return;
    }
    uint64_t t0 = now_ns();
    if (!video_scale_render(&display_scale, src, pitch, scale_format(frame.format), display_pixels,
                            (size_t) surf_stride)) return;
    uint64_t scale_ns = now_ns() - t0;
    pthread_mutex_lock(&video_stats_lock);
    presenter_scale_ns += scale_ns;
    pthread_mutex_unlock(&video_stats_lock);
    fb_show(NULL, display_pixels, display_scale.game_h);
}

static bool presenter_start(unsigned w, unsigned h) {
    if (!w || !h || w > MAX_SURFACE_WIDTH || h > MAX_SURFACE_HEIGHT) return false;
    /* Allocate the current native format; rare geometry/format growth is
     * handled by draining and resizing the queue in present_frame(). */
    size_t bpp = pixfmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4u : 2u;
    size_t bytes = sizeof(native_frame_t) + (size_t) w * h * bpp;
    display_pixels = calloc((size_t) surf_stride * max_game_h, 2);
    if (!display_pixels || !present_start(&presenter, bytes, show_native_frame, NULL)) {
        free(display_pixels);
        display_pixels = NULL;
        return false;
    }
    native_capacity = bytes;
    presenting = true;
    return true;
}

static bool present_frame(const void * data, unsigned w, unsigned h, size_t pitch) {
    size_t row_bytes = (size_t) w * (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4u : 2u);
    if (pitch < row_bytes || pitch > SIZE_MAX / h) return false;
    size_t bytes = sizeof(native_frame_t) + row_bytes * h;
    if (bytes > native_capacity) {
        /* Resizing is rare (e.g. a core enabling an SGB border). Finish the old
         * frame before replacing its queue; allocation never happens per frame. */
        present_stop(&presenter);
        retire_presenter_stats();
        presenting = false;
        if (!present_start(&presenter, bytes, show_native_frame, NULL)) return false;
        native_capacity = bytes;
        presenting = true;
    }
    uint8_t * dst = (uint8_t *) present_buffer(&presenter);
    native_frame_t frame = { w, h, pixfmt };
    memcpy(dst, &frame, sizeof(frame));
    dst += sizeof(frame);
    const uint8_t * src = data;
    for (unsigned y = 0; y < h; y++) memcpy(dst + (size_t) y * row_bytes, src + (size_t) y * pitch, row_bytes);
    present_submit(&presenter, (int) h);
    return true;
}

/* Exclusive framebuffer access for the controls and the pause menu. */
static void fb_claim(void) {
    if (presenting) present_claim(&presenter);
}

static void fb_release(void) {
    if (presenting) present_release(&presenter);
}

static void presenter_stop(void) {
    if (presenting) {
        present_stop(&presenter); /* finish the newest pending frame */
        retire_presenter_stats();
        presenting = false;
    }
    free(display_pixels);
    display_pixels = NULL;
}

#ifdef HOST_BUILD
/* Exercise the real native-frame callback and presenter without /dev/fb0.
 * Mutating the source after submission verifies libretro frame lifetime;
 * changing sizes/formats exercises queue growth and latest-frame draining. */
bool frontend_video_pipeline_check(void) {
    static const struct { unsigned w, h; enum retro_pixel_format format; } cases[] = {
        {160, 144, RETRO_PIXEL_FORMAT_RGB565}, {320, 240, RETRO_PIXEL_FORMAT_RGB565},
        {320, 240, RETRO_PIXEL_FORMAT_XRGB8888}, {256, 224, RETRO_PIXEL_FORMAT_0RGB1555},
        {161, 145, RETRO_PIXEL_FORMAT_RGB565}, {240, 160, RETRO_PIXEL_FORMAT_RGB565}
    };
    if (presenting || fb_fd >= 0 || fb_mem || shadow || capture_frame) return false;
    int saved_w = surf_w, saved_h = surf_h, saved_stride = surf_stride, saved_max = max_game_h;
    int saved_game = game_h, saved_draw = draw_w, saved_x0 = draw_x0;
    int saved_src_w = map_src_w, saved_src_h = map_src_h;
    unsigned saved_last_w = last_src_w, saved_last_h = last_src_h;
    uint64_t saved_frames = video_frames, saved_scale = scale_ns_total;
    enum retro_pixel_format saved_format = pixfmt;
    video_scale_t saved_plan = main_scale;
    bool ok = true;
    surf_w = 480; surf_h = 800; surf_stride = 486; max_game_h = 544;
    fb_pages = 1; fb_page = 0;
    fb_mem = calloc((size_t) surf_stride * surf_h, 2);
    uint16_t * expected = calloc((size_t) surf_stride * max_game_h, 2);
    if (!fb_mem || !expected || !presenter_start(160, 144)) ok = false;
    size_t final_rows = 0;
    for (size_t i = 0; ok && i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned w = cases[i].w, h = cases[i].h;
        pixfmt = cases[i].format;
        size_t bpp = pixfmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4u : 2u;
        size_t pitch = w * bpp + 3;
        uint8_t * allocation = malloc(pitch * h + 1);
        if (!allocation) { ok = false; break; }
        uint8_t * source = allocation + 1;
        for (size_t j = 0; j < pitch * h; j++) source[j] = (uint8_t) (j * 37 + i * 11);
        video_scale_t plan;
        ok = video_scale_init(&plan, (int) w, (int) h, surf_w, max_game_h) &&
             video_scale_render(&plan, source, pitch, scale_format(pixfmt), expected, (size_t) surf_stride);
        final_rows = (size_t) plan.game_h;
        video_cb(source, w, h, pitch);
        memset(source, 0xa5, pitch * h); /* the core may immediately reuse its buffer */
        free(allocation);
        if (quit_flag) ok = false;
    }
    presenter_stop();
    if (ok) {
        for (size_t y = 0; y < final_rows; y++) {
            if (memcmp(fb_mem + y * surf_stride * 2, expected + y * surf_stride,
                       (size_t) surf_w * 2) != 0) ok = false;
            for (int x = surf_w; x < surf_stride; x++)
                if (((uint16_t *) fb_mem)[y * surf_stride + (size_t) x] != 0) ok = false;
        }
    }
    if (ok) {
        uint16_t source[160 * 144];
        for (size_t i = 0; i < 160u * 144u; i++) source[i] = (uint16_t) (i * 13u);
        shadow = calloc((size_t) surf_w * surf_h, 2);
        if (!shadow) ok = false;
        else {
            video_scale_t plan;
            pixfmt = RETRO_PIXEL_FORMAT_RGB565;
            capture_frame = true;
            shadow_rows = 0;
            video_cb(source, 160, 144, 160 * 2);
            capture_frame = false;
            ok = video_scale_init(&plan, 160, 144, surf_w, max_game_h) &&
                 video_scale_render(&plan, source, 160 * 2, VIDEO_SCALE_RGB565,
                                    expected, (size_t) surf_w) &&
                 shadow_rows == plan.game_h &&
                 memcmp(shadow, expected, (size_t) surf_w * plan.game_h * 2) == 0;
            free(shadow); shadow = NULL; shadow_rows = 0;
        }
    }
    free(expected); free(fb_mem); fb_mem = NULL; fb_pages = 0;
    surf_w = saved_w; surf_h = saved_h; surf_stride = saved_stride; max_game_h = saved_max;
    game_h = saved_game; draw_w = saved_draw; draw_x0 = saved_x0;
    map_src_w = saved_src_w; map_src_h = saved_src_h;
    last_src_w = saved_last_w; last_src_h = saved_last_h;
    main_scale = saved_plan; pixfmt = saved_format;
    video_frames = saved_frames; scale_ns_total = saved_scale;
    presenter_scale_ns = 0;
    memset(&retired_presenters, 0, sizeof(retired_presenters));
    return ok;
}
#endif

/* Puts the page the player was showing back on screen. Also called by the
 * watchdog thread: one ioctl on state that is not modified after open. */
static void restore_pan(void) {
    if (fb_fd >= 0) ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo_orig);
}

static void fb_close(void) {
    if (fb_fd < 0) return;
    restore_pan();
    if (fb_mem) munmap(fb_mem, fb_len);
    close(fb_fd);
    fb_fd = -1;
    fb_mem = NULL;
}

/* ---- touch controls ----
 *
 * The control area is every row below the scaled game. Sizes and positions
 * are fractions of the smaller side of that area, so the same layout fits
 * the 480x800, 480x720 and 320x480 panels. All coordinates here are local
 * to the control area. Which buttons exist comes from the emulator's
 * "frontend" settings: face buttons A/B (and X/Y as a diamond) on the right,
 * L and R as wide bars along the top edge, SELECT and START small at the
 * bottom. The D-pad is always there. */

#define COLOR_PANEL 0x18E3
#define COLOR_DPAD 0x39E7
#define COLOR_DPAD_ARM 0x6B4D
#define COLOR_BUTTON_FACE 0xA145
#define COLOR_BUTTON_META 0x6B4D
#define COLOR_LABEL 0xFFFF
#define COLOR_LABEL_DIM 0xC618

#define MAX_FACE 4
#define MAX_META 2
#define MAX_BARS 2

typedef struct {
    int cx, cy, r;
    unsigned bit;
    const char * label;
} pad_button_t;

typedef struct {
    int x0, y0, x1, y1;
    unsigned bit;
    const char * label;
} pad_bar_t;

static struct {
    int top, w, h;                   /* control area on the screen */
    int dpad_cx, dpad_cy, dpad_r, dpad_arm, dpad_dead, dpad_reach;
    pad_button_t face[MAX_FACE];     /* A, B, X, Y as configured */
    int face_count;
    pad_button_t meta[MAX_META];     /* SELECT, START as configured */
    int meta_count;
    pad_bar_t bars[MAX_BARS];        /* L, R */
    int bar_count;
    int button_reach, face_scale, meta_scale, bar_scale, meta_label_dy;
} lay;

static bool has_button(unsigned id) {
    return (cfg.buttons >> id) & 1u;
}

/* Bits the pad reports to the core: the D-pad plus the configured buttons. */
static uint16_t pad_mask(void) {
    uint16_t m = (1u << RETRO_DEVICE_ID_JOYPAD_UP) | (1u << RETRO_DEVICE_ID_JOYPAD_DOWN) |
                 (1u << RETRO_DEVICE_ID_JOYPAD_LEFT) | (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    return (uint16_t) (m | cfg.buttons);
}

static int scale_for(int r, int divisor) {
    return r / divisor > 1 ? r / divisor : 1;
}

static void layout_controls(int top) {
    memset(&lay, 0, sizeof(lay));
    lay.top = top;
    lay.w = surf_w;
    lay.h = surf_h - top;
    /* The smaller side, but at most 85% of the width so the D-pad circle (a quarter of
     * the width from the left edge) stays on screen when the control area is tall. */
    int m = lay.w * 85 / 100 < lay.h ? lay.w * 85 / 100 : lay.h;
    lay.dpad_r = m * 28 / 100;
    lay.dpad_cx = lay.w / 4;
    lay.dpad_cy = lay.h * 46 / 100;
    lay.dpad_arm = lay.dpad_r * 29 / 100;
    lay.dpad_dead = m * 5 / 100;
    lay.dpad_reach = lay.dpad_r + m * 8 / 100;
    lay.button_reach = m * 4 / 100;

    bool diamond = has_button(RETRO_DEVICE_ID_JOYPAD_X) || has_button(RETRO_DEVICE_ID_JOYPAD_Y);
    int face_r = diamond ? m * 10 / 100 : m * 13 / 100;
    lay.face_scale = scale_for(face_r, 8);
    if (diamond) {
        /* SNES positions: Y left, X top, A right, B bottom. */
        int cx = lay.w * 73 / 100, cy = lay.dpad_cy, d = m * 17 / 100;
        static const struct { unsigned bit; int dx, dy; const char * label; } slots[] = {
            { RETRO_DEVICE_ID_JOYPAD_Y, -1, 0, "Y" }, { RETRO_DEVICE_ID_JOYPAD_X, 0, -1, "X" },
            { RETRO_DEVICE_ID_JOYPAD_A, 1, 0, "A" },  { RETRO_DEVICE_ID_JOYPAD_B, 0, 1, "B" },
        };
        for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
            if (has_button(slots[i].bit))
                lay.face[lay.face_count++] = (pad_button_t) { cx + slots[i].dx * d, cy + slots[i].dy * d, face_r,
                                                              slots[i].bit, slots[i].label };
    } else {
        bool a = has_button(RETRO_DEVICE_ID_JOYPAD_A), b = has_button(RETRO_DEVICE_ID_JOYPAD_B);
        if (a)
            lay.face[lay.face_count++] = (pad_button_t) { a && b ? lay.w * 855 / 1000 : lay.w * 74 / 100,
                                                          a && b ? lay.dpad_cy - m * 10 / 100 : lay.dpad_cy,
                                                          face_r, RETRO_DEVICE_ID_JOYPAD_A, "A" };
        if (b)
            lay.face[lay.face_count++] = (pad_button_t) { a && b ? lay.w * 625 / 1000 : lay.w * 74 / 100,
                                                          a && b ? lay.dpad_cy + m * 10 / 100 : lay.dpad_cy,
                                                          face_r, RETRO_DEVICE_ID_JOYPAD_B, "B" };
    }

    int meta_r = m * 85 / 1000;
    lay.meta_scale = (m + 50) / 150;
    lay.meta_label_dy = meta_r + m * 4 / 100 + 7 * lay.meta_scale / 2;
    int meta_cy = lay.h - meta_r - m * 5 / 100;
    bool sel = has_button(RETRO_DEVICE_ID_JOYPAD_SELECT), sta = has_button(RETRO_DEVICE_ID_JOYPAD_START);
    int meta_dx = sel && sta ? lay.w * 104 / 1000 : 0;
    if (sel && sta) {
        /* Half of each label plus one character cell of gap must fit between
         * the two centers, or the labels run into each other. */
        int half_widths = ((6 + 5) * GLYPH5X7_CELL_W - 2 * GLYPH5X7_SPACING) / 2;
        int fit = meta_dx * 2 / (half_widths + GLYPH5X7_CELL_W);
        if (fit < lay.meta_scale) lay.meta_scale = fit > 1 ? fit : 1;
        lay.meta_label_dy = meta_r + m * 4 / 100 + 7 * lay.meta_scale / 2;
    }
    if (sel)
        lay.meta[lay.meta_count++] = (pad_button_t) { lay.w / 2 - meta_dx, meta_cy, meta_r,
                                                      RETRO_DEVICE_ID_JOYPAD_SELECT, "SELECT" };
    if (sta)
        lay.meta[lay.meta_count++] = (pad_button_t) { lay.w / 2 + meta_dx, meta_cy, meta_r,
                                                      RETRO_DEVICE_ID_JOYPAD_START, "START" };

    int gap = m * 12 / 1000 + 2, bar_h = m * 12 / 100;
    lay.bar_scale = scale_for(bar_h, 10);
    if (has_button(RETRO_DEVICE_ID_JOYPAD_L))
        lay.bars[lay.bar_count++] = (pad_bar_t) { gap, gap, lay.w * 42 / 100, gap + bar_h, RETRO_DEVICE_ID_JOYPAD_L, "L" };
    if (has_button(RETRO_DEVICE_ID_JOYPAD_R))
        lay.bars[lay.bar_count++] = (pad_bar_t) { lay.w * 58 / 100, gap, lay.w - gap, gap + bar_h, RETRO_DEVICE_ID_JOYPAD_R, "R" };
}

typedef struct {
    uint16_t * px;
    int w, h;
} canvas_t;

static void fill_rect(canvas_t * c, int x0, int y0, int x1, int y1, uint16_t color) {
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < c->h; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < c->w; x++) c->px[(size_t) y * c->w + x] = color;
}

static void fill_circle(canvas_t * c, int cx, int cy, int r, uint16_t color) {
    for (int dy = -r; dy <= r; dy++) {
        int half = (int) sqrt((double) (r * r - dy * dy));
        fill_rect(c, cx - half, cy + dy, cx + half + 1, cy + dy + 1, color);
    }
}

/* Draws text centered on (cx, cy) at an integer scale. */
static void draw_text(canvas_t * c, int cx, int cy, const char * text, int scale, uint16_t color) {
    int width = (int) strlen(text) * GLYPH5X7_CELL_W * scale - GLYPH5X7_SPACING * scale;
    int x0 = cx - width / 2, y0 = cy - GLYPH5X7_ROWS * scale / 2;
    for (; *text; text++, x0 += GLYPH5X7_CELL_W * scale) {
        const uint8_t * rows = glyph5x7_rows(*text);
        for (int row = 0; row < GLYPH5X7_ROWS; row++)
            for (int col = 0; col < GLYPH5X7_COLS; col++)
                if (rows[row] & (1u << (GLYPH5X7_COLS - 1 - col)))
                    fill_rect(c, x0 + col * scale, y0 + row * scale, x0 + (col + 1) * scale,
                              y0 + (row + 1) * scale, color);
    }
}

static void render_controls(canvas_t * c) {
    fill_rect(c, 0, 0, c->w, c->h, COLOR_PANEL);
    fill_circle(c, lay.dpad_cx, lay.dpad_cy, lay.dpad_r, COLOR_DPAD);
    int inset = lay.dpad_r * 10 / 100;
    fill_rect(c, lay.dpad_cx - lay.dpad_arm, lay.dpad_cy - lay.dpad_r + inset,
              lay.dpad_cx + lay.dpad_arm, lay.dpad_cy + lay.dpad_r - inset, COLOR_DPAD_ARM);
    fill_rect(c, lay.dpad_cx - lay.dpad_r + inset, lay.dpad_cy - lay.dpad_arm,
              lay.dpad_cx + lay.dpad_r - inset, lay.dpad_cy + lay.dpad_arm, COLOR_DPAD_ARM);
    for (int i = 0; i < lay.face_count; i++)
        fill_circle(c, lay.face[i].cx, lay.face[i].cy, lay.face[i].r, COLOR_BUTTON_FACE);
    for (int i = 0; i < lay.meta_count; i++)
        fill_circle(c, lay.meta[i].cx, lay.meta[i].cy, lay.meta[i].r, COLOR_BUTTON_META);
    for (int i = 0; i < lay.bar_count; i++) {
        const pad_bar_t * b = &lay.bars[i];
        fill_rect(c, b->x0, b->y0, b->x1, b->y1, COLOR_BUTTON_META);
        draw_text(c, (b->x0 + b->x1) / 2, (b->y0 + b->y1) / 2, b->label, lay.bar_scale, COLOR_LABEL);
    }
    for (int i = 0; i < lay.face_count; i++)
        draw_text(c, lay.face[i].cx, lay.face[i].cy, lay.face[i].label, lay.face_scale, COLOR_LABEL);
    for (int i = 0; i < lay.meta_count; i++)
        draw_text(c, lay.meta[i].cx, lay.meta[i].cy - lay.meta_label_dy, lay.meta[i].label,
                  lay.meta_scale, COLOR_LABEL_DIM);
}

/* Paints both pages: the game's rows above (black, or the saved frame when
 * the pause menu closes), the controls below. The controls are composed once
 * in RAM and copied a row at a time. */
static bool draw_screen(bool restore_game) {
    canvas_t c = { malloc((size_t) lay.w * lay.h * 2), lay.w, lay.h };
    uint16_t * black = calloc((size_t) surf_w, 2);
    if (!c.px || !black) {
        free(c.px);
        free(black);
        return false;
    }
    render_controls(&c);
    bool game = restore_game && shadow && shadow_rows == lay.top;
    fb_claim();
    for (int page = 0; page < fb_pages; page++) {
        uint16_t * p = fb_page_ptr(page);
        for (int y = 0; y < lay.top; y++)
            memcpy(p + (size_t) y * surf_stride, game ? shadow + (size_t) y * surf_w : black, (size_t) surf_w * 2);
        for (int y = 0; y < lay.h; y++)
            memcpy(p + (size_t) (lay.top + y) * surf_stride, c.px + (size_t) y * c.w, (size_t) c.w * 2);
    }
    fb_release();
    free(c.px);
    free(black);
    return true;
}

/* The game area changed height: move the controls to match. */
static void relayout_for_game(void) {
    if (fb_fd < 0) return;
    layout_controls(game_h);
    if (!draw_screen(false)) runner_log("out of memory");
}

/* Joypad bits pressed by one contact at screen position (x, y). */
static uint16_t touch_bits(int x, int y) {
    y -= lay.top;
    if (y < 0) return 0;
    for (int i = 0; i < lay.bar_count; i++) {
        const pad_bar_t * b = &lay.bars[i];
        int g = lay.button_reach;
        if (x >= b->x0 - g && x < b->x1 + g && y >= b->y0 - g && y < b->y1 + g) return (uint16_t) (1u << b->bit);
    }
    int dx = x - lay.dpad_cx, dy = y - lay.dpad_cy;
    if (dx * dx + dy * dy <= lay.dpad_reach * lay.dpad_reach) {
        int ax = abs(dx), ay = abs(dy);
        if (ax < lay.dpad_dead && ay < lay.dpad_dead) return 0;
        uint16_t bits = 0;
        /* 8-way: a diagonal counts once the minor axis exceeds ~40% of the major. */
        if (ax * 5 >= ay * 2) bits |= 1u << (dx < 0 ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_RIGHT);
        if (ay * 5 >= ax * 2) bits |= 1u << (dy < 0 ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_DOWN);
        return bits;
    }
    /* Round buttons: the nearest one whose reach covers the contact. */
    const pad_button_t * best = NULL;
    int best_d2 = 0;
    for (int i = 0; i < lay.face_count + lay.meta_count; i++) {
        const pad_button_t * b = i < lay.face_count ? &lay.face[i] : &lay.meta[i - lay.face_count];
        int bx = x - b->cx, by = y - b->cy, r = b->r + lay.button_reach, d2 = bx * bx + by * by;
        if (d2 <= r * r && (!best || d2 < best_d2)) {
            best = b;
            best_d2 = d2;
        }
    }
    return best ? (uint16_t) (1u << best->bit) : 0;
}

/* ---- input ---- */

typedef struct {
    int id, x, y;                    /* id < 0: no contact */
    uint64_t seen_ns;                /* type A: last frame that carried it */
} contact_t;

/* The R1 hyn_ts driver sends each contact only on every other panel frame,
 * with an independent phase per contact, so a type A frame often carries
 * one finger of two. A contact missing from a frame is kept for this long
 * and matched to the next frame's positions by distance; lifting the last
 * finger still releases at once (BTN_TOUCH 0). */
#define TYPE_A_HOLD_NS (100 * 1000000ull)
#define TYPE_A_MATCH_PX 120

typedef struct {
    int fd;
    bool touch;                      /* reports touch positions */
    bool multitouch;                 /* ABS_MT_* rather than ABS_X/ABS_Y */
    bool pixel_coords;               /* positions are already panel pixels */
    bool saw_slots;                  /* type B: ABS_MT_SLOT has been seen */
    bool pend_payload;
    struct input_absinfo abs_x, abs_y;
    int cur_slot, pend_x, pend_y, pend_n;
    contact_t contacts[MAX_CONTACTS];
    contact_t pend[MAX_CONTACTS];    /* type A: contacts of the frame being assembled */
} input_dev_t;

static input_dev_t devs[MAX_INPUT_DEVICES];
static int dev_count;
static uint16_t pad_bits;            /* RETRO_DEVICE_ID_JOYPAD_* bitmask */
static uint16_t key_pad_bits;
static bool menu_open;               /* the pause menu is up: the core is not running, no bits reach it */
static bool power_tap;               /* KEY_POWER was pressed and released before the quit hold */
static unsigned char keys_down[(BTN_MISC + 7) / 8];
static int keys_down_count;
static uint64_t power_down_ns;
static int max_touches;

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void) index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t) pad_bits;
    return (pad_bits >> id) & 1;
}

static void input_poll_cb(void) {}

static bool bit_set(const unsigned long * bits, int n) {
    return (bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

/* Physical keys. Volume up/down are A/B and play/pause is START whatever the
 * core. Previous and next are SELECT and A by default (the NES layout); with
 * L/R buttons they become L and R (SELECT stays on the touch screen), and with
 * X/Y buttons but no L/R, next becomes X. A key whose button the emulator does
 * not have does nothing. The power key is the pause menu (and quit on a hold). */
static int key_to_pad_bit(int code) {
    bool shoulders = has_button(RETRO_DEVICE_ID_JOYPAD_L) || has_button(RETRO_DEVICE_ID_JOYPAD_R);
    bool extra_face = has_button(RETRO_DEVICE_ID_JOYPAD_X) || has_button(RETRO_DEVICE_ID_JOYPAD_Y);
    switch (code) {
        case KEY_VOLUMEUP: return RETRO_DEVICE_ID_JOYPAD_A;
        case KEY_VOLUMEDOWN: return RETRO_DEVICE_ID_JOYPAD_B;
        case KEY_PLAYPAUSE: return RETRO_DEVICE_ID_JOYPAD_START;
        case KEY_PREVIOUSSONG: return shoulders ? RETRO_DEVICE_ID_JOYPAD_L : RETRO_DEVICE_ID_JOYPAD_SELECT;
        case KEY_NEXTSONG:
            return shoulders ? RETRO_DEVICE_ID_JOYPAD_R : extra_face ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_A;
        default: return -1;
    }
}

/* The player sets COMPAS_BOARD (r1, r3proii or r3ii_2025); unset means r1. */
static bool on_r1_board(void) {
    const char * board = getenv("COMPAS_BOARD");
    return !board || !*board || strcmp(board, "r1") == 0;
}

/* Opens and grabs every /dev/input/event* so the player sees nothing while
 * the game runs. */
static void input_open(void) {
    DIR * dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent * de;
    while ((de = readdir(dir)) && dev_count < MAX_INPUT_DEVICES) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        input_dev_t * d = &devs[dev_count];
        memset(d, 0, sizeof(*d));
        d->fd = fd;
        for (int i = 0; i < MAX_CONTACTS; i++) d->contacts[i].id = -1;
        unsigned long abs_bits[(ABS_MAX + 1) / (8 * sizeof(long)) + 1] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
        char name[64] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        /* The R1 Hynitron driver advertises 720x1280 ranges but reports
         * panel pixels. The other boards report ranges that scale to the
         * panel. */
        d->pixel_coords = on_r1_board() && strcmp(name, "hyn_ts") == 0;
        if (bit_set(abs_bits, ABS_MT_POSITION_X) && bit_set(abs_bits, ABS_MT_POSITION_Y)) {
            d->touch = d->multitouch = true;
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &d->abs_x);
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &d->abs_y);
        } else if (bit_set(abs_bits, ABS_X) && bit_set(abs_bits, ABS_Y)) {
            d->touch = true;
            ioctl(fd, EVIOCGABS(ABS_X), &d->abs_x);
            ioctl(fd, EVIOCGABS(ABS_Y), &d->abs_y);
        }
        if (ioctl(fd, EVIOCGRAB, 1)) runner_log("grab %s: %s", path, strerror(errno));
        if (d->touch)
            runner_log("touch %s: %s x[%d..%d] y[%d..%d]%s", name, d->multitouch ? "multitouch" : "single",
                       d->abs_x.minimum, d->abs_x.maximum, d->abs_y.minimum, d->abs_y.maximum,
                       d->pixel_coords ? " (pixel coordinates)" : "");
        dev_count++;
    }
    closedir(dir);
}

static void input_close(void) {
    for (int i = 0; i < dev_count; i++) {
        if (devs[i].fd < 0) continue;
        ioctl(devs[i].fd, EVIOCGRAB, 0);
        close(devs[i].fd);
        devs[i].fd = -1;
    }
    dev_count = 0;
}

static void release_contacts(input_dev_t * d) {
    for (int i = 0; i < MAX_CONTACTS; i++) d->contacts[i].id = -1;
    d->pend_n = 0;
    d->pend_payload = false;
}

static int active_contacts(void) {
    int n = 0;
    for (int i = 0; i < dev_count; i++)
        for (int j = 0; j < MAX_CONTACTS; j++) n += devs[i].contacts[j].id >= 0;
    return n;
}

static int scale_axis(const input_dev_t * d, int v, const struct input_absinfo * a, int out) {
    if (d->pixel_coords || a->maximum <= a->minimum) return v;
    return (int) ((int64_t) (v - a->minimum) * (out - 1) / (a->maximum - a->minimum));
}

static void recompute_pad(void) {
    if (menu_open) {
        pad_bits = 0;
        return;
    }
    uint16_t bits = key_pad_bits & pad_mask();
    for (int i = 0; i < dev_count; i++)
        for (int j = 0; j < MAX_CONTACTS; j++) {
            const contact_t * c = &devs[i].contacts[j];
            if (c->id >= 0)
                bits |= touch_bits(scale_axis(&devs[i], c->x, &devs[i].abs_x, surf_w),
                                   scale_axis(&devs[i], c->y, &devs[i].abs_y, surf_h));
        }
    pad_bits = bits;
}

static void handle_key(input_dev_t * d, const struct input_event * ev) {
    if (ev->code == BTN_TOUCH) {
        if (!d->multitouch) d->contacts[0].id = ev->value ? 0 : -1;
        else if (!ev->value) release_contacts(d);
        return;
    }
    if (ev->code == KEY_POWER) {
        /* The hold is measured between the events' own timestamps: a core
         * frame that stalls past the release would otherwise read the press
         * and release together and turn a quit hold into a tap. */
        static uint64_t power_press_ev_us;
#ifdef input_event_sec
        uint64_t ev_us = (uint64_t) ev->input_event_sec * 1000000u + (uint64_t) ev->input_event_usec;
#else
        uint64_t ev_us = (uint64_t) ev->time.tv_sec * 1000000u + (uint64_t) ev->time.tv_usec;
#endif
        if (ev->value == 1) {
            power_down_ns = now_ns();
            power_press_ev_us = ev_us;
        } else if (ev->value == 0) {
            if (power_down_ns && power_press_ev_us && ev_us >= power_press_ev_us &&
                (ev_us - power_press_ev_us) * 1000u >= POWER_HOLD_QUIT_NS) quit_flag = 1;
            else if (power_down_ns && now_ns() - power_down_ns < POWER_HOLD_QUIT_NS) power_tap = true;
            power_down_ns = 0;
            power_press_ev_us = 0;
        }
    }
    if (ev->code < BTN_MISC && ev->value != 2) {
        unsigned char mask = (unsigned char) (1u << (ev->code % 8));
        bool was_down = keys_down[ev->code / 8] & mask;
        if (ev->value && !was_down) { keys_down[ev->code / 8] |= mask; keys_down_count++; }
        else if (!ev->value && was_down) { keys_down[ev->code / 8] &= (unsigned char) ~mask; keys_down_count--; }
    }
    int bit = key_to_pad_bit(ev->code);
    if (bit >= 0) {
        if (ev->value) key_pad_bits |= (uint16_t) (1u << bit);
        else key_pad_bits &= (uint16_t) ~(1u << bit);
    }
}

static void handle_abs(input_dev_t * d, const struct input_event * ev) {
    switch (ev->code) {
        case ABS_MT_SLOT:
            d->saw_slots = true;
            d->cur_slot = (ev->value >= 0 && ev->value < MAX_CONTACTS) ? ev->value : 0;
            break;
        case ABS_MT_TRACKING_ID:
            if (d->saw_slots) d->contacts[d->cur_slot].id = ev->value;
            break;
        case ABS_MT_POSITION_X:
            if (d->saw_slots) d->contacts[d->cur_slot].x = ev->value;
            d->pend_x = ev->value;
            d->pend_payload = true;
            break;
        case ABS_MT_POSITION_Y:
            if (d->saw_slots) d->contacts[d->cur_slot].y = ev->value;
            d->pend_y = ev->value;
            d->pend_payload = true;
            break;
        case ABS_X: if (!d->multitouch) d->contacts[0].x = ev->value; break;
        case ABS_Y: if (!d->multitouch) d->contacts[0].y = ev->value; break;
    }
}

static void merge_type_a_frame(input_dev_t * d) {
    uint64_t now = now_ns();
    bool matched[MAX_CONTACTS] = { false };
    for (int i = 0; i < d->pend_n; i++) {
        const contact_t * p = &d->pend[i];
        int best = -1, best_d2 = TYPE_A_MATCH_PX * TYPE_A_MATCH_PX + 1;
        for (int j = 0; j < MAX_CONTACTS; j++) {
            const contact_t * c = &d->contacts[j];
            if (c->id < 0 || matched[j]) continue;
            int dx = c->x - p->x, dy = c->y - p->y, d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best = j; best_d2 = d2; }
        }
        for (int j = 0; best < 0 && j < MAX_CONTACTS; j++)
            if (d->contacts[j].id < 0) best = j;
        if (best < 0) continue;
        d->contacts[best] = (contact_t) { 0, p->x, p->y, now };
        matched[best] = true;
    }
    for (int j = 0; j < MAX_CONTACTS; j++)
        if (d->contacts[j].id >= 0 && !matched[j] && now - d->contacts[j].seen_ns > TYPE_A_HOLD_NS)
            d->contacts[j].id = -1;
}

/* Drops type A contacts whose hold ran out while no frame arrived. */
static void expire_type_a_contacts(void) {
    uint64_t now = now_ns();
    for (int i = 0; i < dev_count; i++) {
        input_dev_t * d = &devs[i];
        if (!d->multitouch || d->saw_slots) continue;
        for (int j = 0; j < MAX_CONTACTS; j++)
            if (d->contacts[j].id >= 0 && now - d->contacts[j].seen_ns > TYPE_A_HOLD_NS) d->contacts[j].id = -1;
    }
}

/* SYN_REPORT commits a frame. Type A devices (the R1's hyn_ts) have no
 * slots: each contact of the frame was announced by a SYN_MT_REPORT, and an
 * empty frame releases everything. */
static void handle_frame_end(input_dev_t * d) {
    if (!d->touch) return;
    if (d->multitouch && !d->saw_slots) {
        merge_type_a_frame(d);
        d->pend_n = 0;
        d->pend_payload = false;
    }
    int active = active_contacts();
    if (active > max_touches) max_touches = active;
    /* A short trace of contact-count changes, to diagnose a touch panel
     * that reports fewer simultaneous contacts than expected. At most
     * TOUCH_TRACE_LINES lines of under 160 bytes per run. */
    static int logged, last_logged_active;
    if (active != last_logged_active && logged < TOUCH_TRACE_LINES) {
        char line[160];
        int len = snprintf(line, sizeof(line), "touch %s: %d contact(s)", d->saw_slots ? "type B" : "type A", active);
        for (int j = 0; j < MAX_CONTACTS && len > 0 && len < (int) sizeof(line); j++)
            if (d->contacts[j].id >= 0)
                len += snprintf(line + len, sizeof(line) - (size_t) len, " (%d,%d)", d->contacts[j].x, d->contacts[j].y);
        runner_log("%s", line);
        logged++;
    }
    last_logged_active = active;
}

static void handle_event(input_dev_t * d, const struct input_event * ev) {
    if (ev->type == EV_KEY) {
        handle_key(d, ev);
    } else if (ev->type == EV_ABS) {
        if (d->touch) handle_abs(d, ev);
    } else if (ev->type == EV_SYN && ev->code == SYN_MT_REPORT) {
        if (d->pend_payload && d->pend_n < MAX_CONTACTS) d->pend[d->pend_n++] = (contact_t) { 0, d->pend_x, d->pend_y, 0 };
        d->pend_payload = false;
    } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        handle_frame_end(d);
    } else if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
        release_contacts(d);
    }
}

/* ---- touch gestures ----
 *
 * Every touch from the first finger down to the last finger up is one
 * gesture. The pause menu opens on a tap (a short, nearly motionless single
 * touch) in the middle of the game area, and its rows react to a touch that
 * starts and ends in the same row. */

typedef struct {
    bool active, ignore;             /* ignore: a touch already down at start-up never counts */
    uint64_t t0_ns;
    int x0, y0, x, y;                /* start and latest position of the first contact */
    int max_d2, max_contacts;
} gesture_t;

typedef struct {
    bool done;                       /* a gesture ended since it was last consumed */
    bool tap;                        /* it was short, nearly still and single-finger */
    int x0, y0, x, y;
} gesture_result_t;

static gesture_t gesture;
static gesture_result_t gesture_result;

static void track_gesture(void) {
    int n = 0, x = 0, y = 0;
    for (int i = 0; i < dev_count; i++)
        for (int j = 0; j < MAX_CONTACTS; j++) {
            const contact_t * c = &devs[i].contacts[j];
            if (c->id < 0) continue;
            if (!n++) {
                x = scale_axis(&devs[i], c->x, &devs[i].abs_x, surf_w);
                y = scale_axis(&devs[i], c->y, &devs[i].abs_y, surf_h);
            }
        }
    uint64_t now = now_ns();
    if (n > 0 && !gesture.active) {
        gesture = (gesture_t) { .active = true, .ignore = gesture.ignore, .t0_ns = now, .x0 = x, .y0 = y, .x = x, .y = y,
                                .max_contacts = n };
    } else if (n > 0) {
        int dx = x - gesture.x0, dy = y - gesture.y0;
        gesture.x = x;
        gesture.y = y;
        if (dx * dx + dy * dy > gesture.max_d2) gesture.max_d2 = dx * dx + dy * dy;
        if (n > gesture.max_contacts) gesture.max_contacts = n;
    } else if (gesture.active) {
        if (!gesture.ignore)
            gesture_result = (gesture_result_t) {
                .done = true,
                .tap = now - gesture.t0_ns <= TAP_MAX_NS && gesture.max_d2 <= TAP_MAX_MOVE_PX * TAP_MAX_MOVE_PX &&
                       gesture.max_contacts == 1,
                .x0 = gesture.x0, .y0 = gesture.y0, .x = gesture.x, .y = gesture.y,
            };
        gesture.active = gesture.ignore = false;
    }
}

/* Drains every device and refreshes pad_bits; requests quit on a power hold. */
static void input_pump(void) {
    if (power_down_ns && now_ns() - power_down_ns >= POWER_HOLD_QUIT_NS) quit_flag = 1;
    struct pollfd pfds[MAX_INPUT_DEVICES];
    for (int i = 0; i < dev_count; i++) pfds[i] = (struct pollfd) { devs[i].fd, POLLIN, 0 };
    expire_type_a_contacts();
    if (!dev_count || poll(pfds, (nfds_t) dev_count, 0) <= 0) {
        recompute_pad();
        track_gesture();
        return;
    }
    for (int i = 0; i < dev_count; i++) {
        if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
        struct input_event ev[64];
        ssize_t n;
        while ((n = read(devs[i].fd, ev, sizeof(ev))) > 0)
            for (size_t k = 0; k < (size_t) n / sizeof(ev[0]); k++) handle_event(&devs[i], &ev[k]);
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            /* The device went away; its contacts can never be released. */
            release_contacts(&devs[i]);
            close(devs[i].fd);
            devs[i].fd = -1;
        }
    }
    recompute_pad();
    track_gesture();
}

/* Release events must reach this process, not the player: keep reading
 * input, without emulating, until every key is up and no finger is down. */
static void wait_for_release(void) {
    uint64_t deadline = now_ns() + RELEASE_WAIT_NS;
    while (now_ns() < deadline && (keys_down_count > 0 || active_contacts() > 0)) {
        atomic_fetch_add(&heartbeat, 1);
        input_pump();
        usleep(10000);
    }
}

/* ---- core ---- */

static void core_log(enum retro_log_level level, const char * fmt, ...) {
    if (level < RETRO_LOG_WARN) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    while (len && line[len - 1] == '\n') line[--len] = '\0';
    runner_log("core: %s", line);
}

/* ---- frame skipping ----
 *
 * With "frameskip" set, the video of some frames is dropped: video_cb returns
 * at once and the core is told (GET_AUDIO_VIDEO_ENABLE) so it can skip drawing
 * if it supports that. A core that does not can still be given cheaper option
 * answers for those frames ("skip_options"). The audio of every frame is kept. */

static bool skip_pending;                /* the next frame's video is skipped */

static void set_video_skip(bool skip) {
    if (skip == video_skip) return;
    video_skip = skip;
    if (cfg.skip_option_count) variables_dirty = true;
}

/* Called before each retro_run. */
static void frameskip_begin(void) {
    set_video_skip(cfg.frameskip != FRAMESKIP_OFF && skip_pending);
}

/* Called after each retro_run with the time it took and the frame budget. */
static void frameskip_end(uint64_t run_ns, uint64_t budget_ns) {
    bool was_skipped = video_skip;
    if (cfg.frameskip == FRAMESKIP_AUTO) skip_pending = !was_skipped && run_ns > budget_ns;
    else if (cfg.frameskip == FRAMESKIP_HALF) skip_pending = !was_skipped;
    else skip_pending = false;
}

/* Runs that must show their frame (the pause menu capture, state checks). */
static void frameskip_suspend(void) {
    skip_pending = false;
    set_video_skip(false);
}

/* RETRO_AV_ENABLE_VIDEO and _AUDIO; the enum is missing from older libretro.h copies. */
#define AV_ENABLE_VIDEO 1
#define AV_ENABLE_AUDIO 2

static bool av_pending;                /* SET_SYSTEM_AV_INFO arrived: the main loop re-reads timing */

static bool env_cb(unsigned cmd, void * data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            enum retro_pixel_format f = *(const enum retro_pixel_format *) data;
            if (f != RETRO_PIXEL_FORMAT_RGB565 && f != RETRO_PIXEL_FORMAT_XRGB8888 &&
                f != RETRO_PIXEL_FORMAT_0RGB1555) return false;
            pixfmt = f;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            *(const char **) data = emu_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **) data = save_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((struct retro_log_callback *) data)->log = core_log;
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool *) data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable * var = data;
            const char * value = NULL;
            if (var && var->key) {
                if (video_skip) value = frontend_config_skip_option(&cfg, var->key);
                if (!value) value = frontend_config_option(&cfg, var->key);
            }
            if (var) var->value = value;
            return value != NULL; /* unknown keys: the core keeps its own default */
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool *) data = variables_dirty;
            variables_dirty = false;
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            /* Only cores of emulators that use frameskip are told; the others see the call as unsupported. */
            if (cfg.frameskip == FRAMESKIP_OFF) return false;
            *(int *) data = AV_ENABLE_AUDIO | (video_skip ? 0 : AV_ENABLE_VIDEO);
            return true;
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            return true; /* the next frame's size drives the layout */
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
            av_pending = true;
            return true;
        /* Accepted and ignored: the frontend has no option menu, the answers
         * come from emulator.json. */
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
            return true;
        default:
            return false;
    }
}

static void * read_file(const char * path, size_t * len) {
    *len = 0;
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    void * buf = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t) n);
        if (buf && fread(buf, 1, (size_t) n, f) != (size_t) n) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    if (buf) *len = (size_t) n;
    return buf;
}

/* "<rom without extension><suffix>", next to the ROM. */
static void save_path(const char * rom, const char * suffix, char * out, size_t size) {
    snprintf(out, size, "%s", rom);
    char * slash = strrchr(out, '/');
    char * dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    size_t len = strlen(out);
    snprintf(out + len, size - len, "%s", suffix);
}

static void sram_load(const char * rom) {
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void * mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!n || !mem) return;
    char path[640];
    save_path(rom, ".srm", path, sizeof(path));
    FILE * f = fopen(path, "rb");
    if (!f) return;
    if (fread(mem, 1, n, f) != n) runner_log("sram: short read from %s", path);
    fclose(f);
}

/* Temp file plus rename, so a power loss never leaves a truncated save. */
static bool write_file_atomic(const char * path, const void * mem, size_t n) {
    char tmp[648];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "wb");
    if (!f) {
        runner_log("cannot write %s: %s", tmp, strerror(errno));
        return false;
    }
    bool ok = fwrite(mem, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        runner_log("saving %s failed", path);
        unlink(tmp);
        return false;
    }
    return true;
}

static void sram_save(const char * rom) {
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    const void * mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!n || !mem) return;
    char path[640];
    save_path(rom, ".srm", path, sizeof(path));
    write_file_atomic(path, mem, n);
}

/* One save state slot, "<rom without extension>.state". */
static bool state_supported(void) {
    return retro_serialize_size() > 0;
}

static bool state_exists(const char * rom) {
    char path[640];
    struct stat st;
    save_path(rom, ".state", path, sizeof(path));
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool state_save(const char * rom) {
    size_t n = retro_serialize_size();
    void * buf = n ? malloc(n) : NULL;
    char path[640];
    save_path(rom, ".state", path, sizeof(path));
    bool ok = buf && retro_serialize(buf, n) && write_file_atomic(path, buf, n);
    free(buf);
    return ok;
}

static bool state_load(const char * rom) {
    char path[640];
    save_path(rom, ".state", path, sizeof(path));
    size_t len;
    void * buf = read_file(path, &len);
    bool ok = buf && len == retro_serialize_size() && retro_unserialize(buf, len);
    free(buf);
    return ok;
}

static void core_start(void) {
    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_sample_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();
}

/* Loads the ROM into the core. The ROM data must stay alive until
 * core_stop(), so it is returned for the caller to free afterwards. */
static bool core_load(const char * rom, struct retro_system_av_info * av, void ** rom_data) {
    struct retro_system_info si;
    retro_get_system_info(&si);
    struct retro_game_info gi = { .path = rom };
    size_t rom_len = 0;
    *rom_data = NULL;
    if (!si.need_fullpath) {
        *rom_data = read_file(rom, &rom_len);
        if (!*rom_data) {
            runner_log("cannot read %s", rom);
            return false;
        }
        gi.data = *rom_data;
        gi.size = rom_len;
    }
    if (!retro_load_game(&gi)) {
        runner_log("core refused %s", rom);
        free(*rom_data);
        *rom_data = NULL;
        return false;
    }
    variables_dirty = true;
    retro_get_system_av_info(av);
    runner_log("core %s %s: %ux%u @ %.3f fps, audio %.0f Hz, pixel format %d", si.library_name,
               si.library_version, av->geometry.base_width, av->geometry.base_height, av->timing.fps,
               av->timing.sample_rate, (int) pixfmt);
    return true;
}

static void core_stop(void * rom_data) {
    retro_unload_game();
    retro_deinit();
    free(rom_data);
}

static void set_save_dir(const char * rom) {
    snprintf(save_dir, sizeof(save_dir), "%s", rom);
    char * slash = strrchr(save_dir, '/');
    if (slash) *slash = '\0';
    else strcpy(save_dir, ".");
}

/* ---- emulator settings ---- */

static bool settings_loaded;

static bool file_exists(const char * path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* The player starts the emulator with its own folder as the working
 * directory; run by hand from elsewhere, the folder of the executable is the
 * fallback. That folder is the system directory and holds emulator.json. */
static void load_settings(const char * argv0) {
    char path[640];
    settings_loaded = true;
    frontend_config_default(&cfg);
    if (!getcwd(emu_dir, sizeof(emu_dir))) snprintf(emu_dir, sizeof(emu_dir), ".");
    snprintf(path, sizeof(path), "%s/emulator.json", emu_dir);
    if (!file_exists(path) && argv0 && strrchr(argv0, '/')) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", argv0);
        *strrchr(dir, '/') = '\0';
        snprintf(path, sizeof(path), "%s/emulator.json", dir[0] ? dir : "/");
        if (file_exists(path)) snprintf(emu_dir, sizeof(emu_dir), "%s", dir[0] ? dir : "/");
    }
    size_t len;
    char * json = read_file(path, &len);
    if (!json) {
        runner_log("no emulator.json in %s: default settings", emu_dir);
        return;
    }
    char why[64];
    if (!frontend_config_parse(json, len, &cfg, why, sizeof(why))) {
        runner_log("emulator.json: %s: default settings", why);
        frontend_config_default(&cfg);
    }
    free(json);
}

/* ---- headless benchmark ---- */

static long status_kb(const char * key) {
    FILE * f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    size_t key_len = strlen(key);
    while (f && fgets(line, sizeof(line), f))
        if (!strncmp(line, key, key_len)) {
            v = strtol(line + key_len + 1, NULL, 10);
            break;
        }
    if (f) fclose(f);
    return v;
}

/* Scripted input: get past title screens, then keep playing. */
static uint16_t bench_input(uint64_t frame) {
    uint64_t f = frame % 600;
    uint16_t bits = 0;
    if (frame < 900) {
        if ((frame >= 60 && frame < 66) || (frame >= 240 && frame < 246)) bits |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
        if (frame % 170 < 6 && frame > 100) bits |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
        return bits;
    }
    bits |= 1u << (f < 300 ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (f % 20 < 4) bits |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
    if (f % 45 < 3) bits |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
    return bits;
}

static uint32_t fnv1a(uint32_t h, const void * data, size_t n) {
    const uint8_t * p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* Runs a few frames after the timed ones and hashes what the core produced
 * (its audio before resampling, then the last scaled picture), so builds with
 * other compiler flags can be compared for identical output. Not timed. */
#define BENCH_CHECK_FRAMES 30
static void bench_check_frames(uint64_t first_frame, uint32_t * audio_hash, uint32_t * video_hash, int * peak) {
    uint32_t ah = 2166136261u, vh = 2166136261u;
    frameskip_suspend();
    for (int i = 0; i < BENCH_CHECK_FRAMES; i++) {
        pad_bits = bench_input(first_frame + (uint64_t) i);
        retro_run();
        ah = fnv1a(ah, audio_buf, audio_buf_frames * 4);
        for (size_t k = 0; k < audio_buf_frames * 2; k++) {
            int v = audio_buf[k] < 0 ? -audio_buf[k] : audio_buf[k];
            if (v > *peak) *peak = v;
        }
        audio_flush();
    }
    for (int y = 0; y < game_h; y++) vh = fnv1a(vh, surf + (size_t) y * surf_stride, (size_t) surf_w * 2);
    *audio_hash = ah;
    *video_hash = vh;
}

/* Serialize, run on, unserialize, serialize again: the two states must be
 * identical. 1 = round trip works, 0 = it does not, -1 = no state support. */
static int state_roundtrip_check(uint64_t first_frame, size_t * size) {
    *size = retro_serialize_size();
    if (!*size) return -1;
    void * a = malloc(*size);
    void * b = malloc(*size);
    bool ok = a && b && retro_serialize(a, *size);
    for (int i = 0; ok && i < 30; i++) {
        pad_bits = bench_input(first_frame + (uint64_t) i);
        retro_run();
        audio_flush();
    }
    ok = ok && retro_unserialize(a, *size) && retro_serialize(b, *size);
    if (ok && memcmp(a, b, *size) != 0) {
        size_t first = 0, differing = 0;
        for (size_t i = 0; i < *size; i++)
            if (((const uint8_t *) a)[i] != ((const uint8_t *) b)[i] && !differing++) first = i;
        runner_log("state differs after a round trip: %zu bytes, first at offset %zu", differing, first);
        ok = false;
    }
    free(a);
    free(b);
    return ok ? 1 : 0;
}

static uint64_t bench_budget_ns;     /* --budget-ms: pretend a slower device for the frameskip policy */

bool frontend_bench(const char * rom, long frames, frontend_bench_t * out) {
    memset(out, 0, sizeof(*out));
    if (!settings_loaded) load_settings(NULL);
    set_save_dir(rom);
    core_start();
    struct retro_system_av_info av;
    void * rom_data;
    if (!core_load(rom, &av, &rom_data)) {
        retro_deinit();
        return false;
    }
    (void) resample_init((unsigned) (av.timing.sample_rate + 0.5)); /* the resampler is part of the cost */
    surf_w = 480;
    surf_h = 800;
    surf_stride = 480;
    max_game_h = surf_h * (int) cfg.game_height_percent / 100;
    surf = calloc((size_t) surf_stride * surf_h, 2);
    uint64_t budget_ns = bench_budget_ns ? bench_budget_ns : (uint64_t) (1e9 / av.timing.fps);
    uint64_t audio_ns = 0, t_start = now_ns();
    double c_start = cpu_seconds();
    for (long f = 0; f < frames; f++) {
        pad_bits = bench_input((uint64_t) f);
        frameskip_begin();
        uint64_t t0 = now_ns();
        retro_run();
        uint64_t t1 = now_ns();
        audio_flush();
        audio_ns += now_ns() - t1;
        frameskip_end(t1 - t0, budget_ns);
    }
    out->wall_s = (double) (now_ns() - t_start) / 1e9;
    out->audio_s = (double) audio_ns / 1e9;
    out->scale_s = (double) scale_ns_total / 1e9;
    frameskip_suspend();
    out->cpu_s = cpu_seconds() - c_start;
    out->width = last_src_w;
    out->height = last_src_h;
    out->scaled_width = draw_w;
    out->scaled_height = game_h;
    out->sample_rate = av.timing.sample_rate;
    out->fps = av.timing.fps;
    out->frames = (uint64_t) frames;
    out->video_frames = video_frames;
    out->video_skipped = video_skipped;
    out->audio_frames = audio_frames_total;
    out->vm_hwm_kb = status_kb("VmHWM");
    out->sram_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    out->audio_peak = 0;
    bench_check_frames((uint64_t) frames, &out->audio_hash, &out->video_hash, &out->audio_peak);
    out->state_roundtrip = state_roundtrip_check((uint64_t) frames + BENCH_CHECK_FRAMES, &out->state_size);
    core_stop(rom_data);
    free(surf);
    surf = NULL;
    return true;
}

static int run_bench(const char * rom, long frames) {
    frontend_bench_t r;
    if (!frontend_bench(rom, frames, &r)) return 1;
    printf("frames %ld  wall %.2fs  cpu %.2fs  speed %.1f fps (%.0f%% of real time)\n", frames, r.wall_s, r.cpu_s,
           frames / r.wall_s, 100.0 * frames / r.wall_s / r.fps);
    printf("avg %.2f ms/frame (budget %.2f)  scale avg %.2f ms  audio %.0f Hz (core reports %.0f)\n",
           r.wall_s * 1000 / frames, 1000 / r.fps,
           (double) scale_ns_total / 1e6 / (double) (r.video_frames ? r.video_frames : 1),
           (double) r.audio_frames / ((double) frames / r.fps), r.sample_rate);
    printf("video %ux%u scaled to %dx%d  VmHWM %ld kB\n", r.width, r.height, r.scaled_width, r.scaled_height,
           r.vm_hwm_kb);
    printf("rendered %llu of %ld frames (video skipped on %llu)  audio resampling avg %.2f ms\n",
           (unsigned long long) r.video_frames, frames, (unsigned long long) r.video_skipped,
           r.audio_s * 1000 / frames);
    printf("output check: audio %08x video %08x, audio peak %d\n", r.audio_hash, r.video_hash, r.audio_peak);
    printf("save RAM %zu bytes, state %zu bytes, state round trip %s\n", r.sram_size, r.state_size,
           r.state_roundtrip > 0 ? "ok" : r.state_roundtrip == 0 ? "FAILED" : "unsupported");
    return 0;
}

/* ---- layout self-check ---- */

/* Lays the controls out for a screen and a set of buttons and checks that
 * everything fits inside the control area, nothing overlaps, and touching the
 * middle of each control presses exactly that control. */
bool frontend_layout_check(int width, int height, int game_height, uint32_t buttons, char * error, size_t error_size) {
    int saved_w = surf_w, saved_h = surf_h;
    frontend_config_t saved_cfg = cfg;
    surf_w = width;
    surf_h = height;
    cfg.buttons = buttons;
    layout_controls(game_height);
    const char * why = NULL;
    char detail[96] = "";

    /* Each control is a circle (r > 0) or a rectangle (r == 0). */
    typedef struct { int x0, y0, x1, y1, r; } shape_t;
    shape_t shapes[1 + MAX_FACE + MAX_META + MAX_BARS];
    unsigned bits[1 + MAX_FACE + MAX_META + MAX_BARS];
    const char * names[1 + MAX_FACE + MAX_META + MAX_BARS];
    int n = 0;
    shapes[n] = (shape_t) { lay.dpad_cx - lay.dpad_r, lay.dpad_cy - lay.dpad_r, lay.dpad_cx + lay.dpad_r, lay.dpad_cy + lay.dpad_r, lay.dpad_r };
    bits[n] = 99;
    names[n++] = "D-pad";
    for (int i = 0; i < lay.face_count + lay.meta_count; i++, n++) {
        const pad_button_t * b = i < lay.face_count ? &lay.face[i] : &lay.meta[i - lay.face_count];
        shapes[n] = (shape_t) { b->cx - b->r, b->cy - b->r, b->cx + b->r, b->cy + b->r, b->r };
        bits[n] = b->bit;
        names[n] = b->label;
    }
    for (int i = 0; i < lay.bar_count; i++, n++) {
        const pad_bar_t * b = &lay.bars[i];
        shapes[n] = (shape_t) { b->x0, b->y0, b->x1, b->y1, 0 };
        bits[n] = b->bit;
        names[n] = b->label;
    }
    for (int i = 0; !why && i < n; i++) {
        if (shapes[i].x0 < 0 || shapes[i].y0 < 0 || shapes[i].x1 > lay.w || shapes[i].y1 > lay.h) {
            why = "outside the control area";
            snprintf(detail, sizeof(detail), "%s", names[i]);
        }
        for (int j = i + 1; !why && j < n; j++) {
            const shape_t *p = &shapes[i], *q = &shapes[j];
            bool hit;
            if (p->r && q->r) {
                long dx = (p->x0 + p->x1) / 2 - (q->x0 + q->x1) / 2, dy = (p->y0 + p->y1) / 2 - (q->y0 + q->y1) / 2;
                hit = dx * dx + dy * dy < (long) (p->r + q->r) * (p->r + q->r);
            } else if (p->r || q->r) {
                const shape_t * circle = p->r ? p : q;
                const shape_t * rect = p->r ? q : p;
                int cx = (circle->x0 + circle->x1) / 2, cy = (circle->y0 + circle->y1) / 2;
                int nx = cx < rect->x0 ? rect->x0 : cx > rect->x1 ? rect->x1 : cx;
                int ny = cy < rect->y0 ? rect->y0 : cy > rect->y1 ? rect->y1 : cy;
                hit = (long) (cx - nx) * (cx - nx) + (long) (cy - ny) * (cy - ny) < (long) circle->r * circle->r;
            } else {
                hit = p->x0 < q->x1 && q->x0 < p->x1 && p->y0 < q->y1 && q->y0 < p->y1;
            }
            if (hit) {
                why = "controls overlap";
                snprintf(detail, sizeof(detail), "%s and %s", names[i], names[j]);
            }
        }
    }
    for (int i = 1; !why && i < n; i++) {
        int cx = (shapes[i].x0 + shapes[i].x1) / 2, cy = (shapes[i].y0 + shapes[i].y1) / 2;
        if (touch_bits(cx, cy + lay.top) != (uint16_t) (1u << bits[i])) {
            why = "touching the middle of a control does not press it";
            snprintf(detail, sizeof(detail), "%s", names[i]);
        }
    }
    if (!why && touch_bits(lay.dpad_cx + lay.dpad_r * 6 / 10, lay.dpad_cy + lay.top) != (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT)) {
        why = "the D-pad right arm does not press RIGHT";
    }
    if (why && error && error_size) snprintf(error, error_size, "%dx%d game %d buttons 0x%x: %s %s", width, height, game_height, buttons, why, detail);
    surf_w = saved_w;
    surf_h = saved_h;
    cfg = saved_cfg;
    return !why;
}

/* ---- pause menu ----
 *
 * Opened by a tap in the middle of the game or a short press of the power
 * key. While it is up the core does not run, joypad bits are cleared and the
 * audio device gets silence, so nothing buzzes and the watchdog heartbeat
 * keeps advancing. The game frame it opened over is dimmed behind the rows. */

enum { ITEM_RESUME, ITEM_SAVE, ITEM_LOAD, ITEM_RESET, ITEM_EXIT, ITEM_COUNT };
static const char * const item_label[ITEM_COUNT] = { "RESUME", "SAVE STATE", "LOAD STATE", "RESET", "EXIT" };

#define MENU_STATUS_NS 1200000000ull
#define COLOR_MENU_ROW 0x2965
#define COLOR_MENU_EDGE 0x8410
#define COLOR_MENU_OFF 0x5AEB

static struct {
    bool enabled[ITEM_COUNT];
    int x0, x1, y0[ITEM_COUNT], row_h, scale;
    int status_y;
    char status[16];
    uint64_t status_until_ns;
} menu;

static const char * current_rom;
static bool resume_pending;            /* the main loop re-syncs its frame clock */

static void dim_565(uint16_t * px, size_t n) {
    for (size_t i = 0; i < n; i++) px[i] = (uint16_t) ((px[i] >> 1) & 0x7BEF);
}

static void menu_layout(void) {
    menu.row_h = surf_h * 9 / 100;
    int gap = surf_h * 15 / 1000 + 2;
    int total = ITEM_COUNT * menu.row_h + (ITEM_COUNT - 1) * gap;
    int top = (surf_h - total) / 2 - menu.row_h / 2;
    if (top < gap) top = gap;
    menu.x0 = surf_w * 15 / 100;
    menu.x1 = surf_w - menu.x0;
    for (int i = 0; i < ITEM_COUNT; i++) menu.y0[i] = top + i * (menu.row_h + gap);
    menu.status_y = top + total + gap + menu.row_h / 2;
    int by_height = menu.row_h * 55 / 100 / GLYPH5X7_ROWS;
    int by_width = (menu.x1 - menu.x0) * 85 / 100 / (10 * GLYPH5X7_CELL_W - GLYPH5X7_SPACING);
    menu.scale = by_height < by_width ? by_height : by_width;
    if (menu.scale < 1) menu.scale = 1;
}

static void menu_refresh_items(void) {
    bool states = state_supported();
    menu.enabled[ITEM_RESUME] = menu.enabled[ITEM_RESET] = menu.enabled[ITEM_EXIT] = true;
    menu.enabled[ITEM_SAVE] = states;
    menu.enabled[ITEM_LOAD] = states && state_exists(current_rom);
}

static int menu_row_at(int x, int y) {
    if (x < menu.x0 || x >= menu.x1) return -1;
    for (int i = 0; i < ITEM_COUNT; i++)
        if (y >= menu.y0[i] && y < menu.y0[i] + menu.row_h) return i;
    return -1;
}

/* Draws the dimmed backdrop (the saved game frame and the controls) and the
 * menu rows over it into a full-screen canvas. */
static void menu_compose(canvas_t * cv) {
    canvas_t c = *cv;
    for (int y = 0; y < lay.top; y++) {
        uint16_t * row = c.px + (size_t) y * surf_w;
        if (shadow && y < shadow_rows) {
            memcpy(row, shadow + (size_t) y * surf_w, (size_t) surf_w * 2);
            dim_565(row, (size_t) surf_w);
        } else {
            memset(row, 0, (size_t) surf_w * 2);
        }
    }
    canvas_t controls = { c.px + (size_t) lay.top * surf_w, surf_w, lay.h };
    render_controls(&controls);
    dim_565(controls.px, (size_t) surf_w * lay.h);
    for (int i = 0; i < ITEM_COUNT; i++) {
        int y0 = menu.y0[i], y1 = y0 + menu.row_h;
        fill_rect(&c, menu.x0, y0, menu.x1, y1, COLOR_MENU_EDGE);
        fill_rect(&c, menu.x0 + 2, y0 + 2, menu.x1 - 2, y1 - 2, COLOR_MENU_ROW);
        draw_text(&c, (menu.x0 + menu.x1) / 2, (y0 + y1) / 2, item_label[i], menu.scale,
                  menu.enabled[i] ? COLOR_LABEL : COLOR_MENU_OFF);
    }
    if (menu.status[0]) draw_text(&c, surf_w / 2, menu.status_y, menu.status, menu.scale, COLOR_LABEL);
}

/* Composes the whole screen in RAM and copies it to both pages. */
static bool menu_render(void) {
    canvas_t c = { malloc((size_t) surf_w * surf_h * 2), surf_w, surf_h };
    if (!c.px) return false;
    menu_compose(&c);
    fb_claim();
    for (int page = 0; page < fb_pages; page++) {
        uint16_t * p = fb_page_ptr(page);
        for (int y = 0; y < surf_h; y++) memcpy(p + (size_t) y * surf_stride, c.px + (size_t) y * surf_w, (size_t) surf_w * 2);
    }
    fb_release();
    free(c.px);
    return true;
}

static void menu_set_status(const char * text) {
    snprintf(menu.status, sizeof(menu.status), "%s", text);
    menu.status_until_ns = now_ns() + MENU_STATUS_NS;
}

/* Runs the core until a clean frame is kept for the dimmed backdrop, then
 * shows the menu. (A frame the core duplicates is not delivered, so a couple
 * of extra frames may run; nothing is shown meanwhile.) */
static void menu_begin(void) {
    shadow = malloc((size_t) surf_w * surf_h * 2);
    if (!shadow) return;
    shadow_rows = 0;
    frameskip_suspend();
    capture_frame = true;
    for (int i = 0; i < 4 && !shadow_rows; i++) {
        retro_run();
        audio_flush();
    }
    capture_frame = false;
    pad_bits = 0;
    menu_layout();
    menu_refresh_items();
    menu.status[0] = '\0';
    menu_open = true;
    if (!menu_render()) menu_open = false;
    if (!menu_open) {
        free(shadow);
        shadow = NULL;
        shadow_rows = 0;
    }
}

static void menu_close(void) {
    menu_open = false;
    resume_pending = true;
    if (!draw_screen(true)) runner_log("out of memory");
    free(shadow);
    shadow = NULL;
    shadow_rows = 0;
}

static void menu_activate(int item) {
    switch (item) {
        case ITEM_RESUME:
            menu_close();
            break;
        case ITEM_SAVE:
            menu_set_status(state_save(current_rom) ? "SAVED" : "FAILED");
            menu_refresh_items();
            menu_render();
            break;
        case ITEM_LOAD:
            if (state_load(current_rom)) {
                menu_close();
            } else {
                menu_set_status("FAILED");
                menu_render();
            }
            break;
        case ITEM_RESET:
            retro_reset();
            menu_close();
            break;
        case ITEM_EXIT:
            quit_flag = 1;
            break;
    }
}

/* One pass of the paused loop: react to the power key and to touches, keep
 * the audio device fed with silence. */
static void menu_step(void) {
    if (power_tap) {
        power_tap = false;
        menu_close();
        return;
    }
    if (gesture_result.done) {
        gesture_result_t g = gesture_result;
        gesture_result.done = false;
        int row = menu_row_at(g.x0, g.y0);
        if (row >= 0 && row == menu_row_at(g.x, g.y) && menu.enabled[row]) {
            menu_activate(row);
            return;
        }
    }
    if (menu.status[0] && now_ns() >= menu.status_until_ns) {
        menu.status[0] = '\0';
        menu_render();
    }
    if (!audio_silence()) usleep(16000);
}

/* A short, still, single-finger touch in the middle third of the game area. */
static bool tap_in_game_center(const gesture_result_t * g) {
    if (!g->tap || lay.top <= 0) return false;
    int x_lo = surf_w / 3, x_hi = surf_w * 2 / 3, y_lo = lay.top / 3, y_hi = lay.top * 2 / 3;
    return g->x0 >= x_lo && g->x0 < x_hi && g->y0 >= y_lo && g->y0 < y_hi;
}

/* Writes the touch layout, or the pause menu over it, for a screen size to a
 * PPM file, so the drawing can be looked at without a device. The game area is
 * a flat grey stand-in. */
bool frontend_preview(int width, int height, int game_height, uint32_t buttons, bool with_menu, const char * ppm_path) {
    int saved_w = surf_w, saved_h = surf_h;
    frontend_config_t saved_cfg = cfg;
    surf_w = width;
    surf_h = height;
    cfg.buttons = buttons;
    layout_controls(game_height);
    canvas_t c = { calloc((size_t) width * height, 2), width, height };
    bool ok = c.px != NULL;
    if (ok) {
        canvas_t controls = { c.px + (size_t) lay.top * width, width, lay.h };
        for (int y = 0; y < lay.top; y++)
            for (int x = 0; x < width; x++) c.px[(size_t) y * width + x] = (uint16_t) (0x4208 + ((x / 16 + y / 16) & 1) * 0x0841);
        render_controls(&controls);
        if (with_menu) {
            current_rom = "";
            menu_layout();
            for (int i = 0; i < ITEM_COUNT; i++) menu.enabled[i] = i != ITEM_LOAD;
            snprintf(menu.status, sizeof(menu.status), "SAVED");
            shadow = c.px; /* the stand-in game rows are the backdrop */
            shadow_rows = lay.top;
            canvas_t out = { calloc((size_t) width * height, 2), width, height };
            if (out.px) {
                menu_compose(&out);
                free(c.px);
                c = out;
            }
            shadow = NULL;
            shadow_rows = 0;
        }
        FILE * f = fopen(ppm_path, "wb");
        ok = f != NULL;
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", width, height);
            for (size_t i = 0; i < (size_t) width * height; i++) {
                uint16_t p = c.px[i];
                uint8_t rgb[3] = { (uint8_t) (((p >> 11) & 31) * 255 / 31), (uint8_t) (((p >> 5) & 63) * 255 / 63), (uint8_t) ((p & 31) * 255 / 31) };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
    free(c.px);
    surf_w = saved_w;
    surf_h = saved_h;
    cfg = saved_cfg;
    return ok;
}

/* ---- interactive session ---- */

static void on_quit_signal(int sig) {
    (void) sig;
    quit_flag = 1;
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_quit_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

/* If the main loop stops advancing (a stuck audio write, a runaway core),
 * give the screen back and die so the player can carry on. */
static void * watchdog_main(void * arg) {
    (void) arg;
    unsigned last = atomic_load(&heartbeat);
    int stalled = 0;
    for (;;) {
        sleep(1);
        unsigned now = atomic_load(&heartbeat);
        stalled = now == last ? stalled + 1 : 0;
        last = now;
        if (stalled >= WATCHDOG_STALL_SECONDS) {
            runner_log("main loop stalled for %d s, aborting", stalled);
            restore_pan();
            _exit(3);
        }
    }
    return NULL;
}

static void start_watchdog(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, watchdog_main, NULL) == 0) pthread_detach(thread);
}

static void interactive_stats(uint64_t interval_ns, uint64_t * last_time, uint64_t * last_frames,
                              uint64_t * last_core_ns, uint64_t * last_scale_ns,
                              uint64_t * last_audio_ns, double * last_cpu_s, unsigned * last_underruns,
                              uint64_t frames, uint64_t core_ns, uint64_t audio_ns,
                              double target_fps) {
    uint64_t now = now_ns();
    if (!interval_ns || now - *last_time < interval_ns) return;
    double elapsed = (double) (now - *last_time) / 1e9;
    uint64_t interval_frames = frames - *last_frames;
    uint64_t core_delta = core_ns - *last_core_ns;
    uint64_t scaling_now = video_scaling_ns();
    uint64_t scale_delta = scaling_now - *last_scale_ns;
    uint64_t audio_delta = audio_ns - *last_audio_ns;
    double cpu_now = cpu_seconds();
    double cpu_percent = elapsed > 0 ? 100.0 * (cpu_now - *last_cpu_s) / elapsed : 0.0;
    double n = interval_frames ? (double) interval_frames : 1.0;
    long rss_kb = status_kb("VmRSS");
    long swap_kb = status_kb("VmSwap");
    runner_log("stats: %llu frames/%.1fs = %.1f emulated fps (target %.1f), cpu %.0f%%, core %.2f ms/frame, "
               "scale %.2f ms/frame, audio write/pacing %.2f ms/frame, underruns +%u, RSS %ld KiB, Swap %ld KiB",
               (unsigned long long) interval_frames, elapsed,
               elapsed > 0 ? (double) interval_frames / elapsed : 0.0, target_fps, cpu_percent,
               (double) core_delta / 1e6 / n, (double) scale_delta / 1e6 / n,
               (double) audio_delta / 1e6 / n, audio_underrun_count() - *last_underruns, rss_kb, swap_kb);
    *last_time = now;
    *last_frames = frames;
    *last_core_ns = core_ns;
    *last_scale_ns = scaling_now;
    *last_audio_ns = audio_ns;
    *last_cpu_s = cpu_now;
    *last_underruns = audio_underrun_count();
}

static int run_interactive(const char * rom, unsigned stats_interval) {
    set_save_dir(rom);
    current_rom = rom;
    core_start();
    struct retro_system_av_info av;
    void * rom_data;
    if (!core_load(rom, &av, &rom_data)) {
        retro_deinit();
        return 1;
    }
    if (!fb_open()) {
        fb_close();
        core_stop(rom_data);
        return 1;
    }
    sram_load(rom);
    max_game_h = surf_h * (int) cfg.game_height_percent / 100;
    build_maps((int) av.geometry.base_width, (int) av.geometry.base_height);
    layout_controls(game_h);
    if (!draw_screen(false)) {
        runner_log("out of memory");
        fb_close();
        core_stop(rom_data);
        return 1;
    }
    if (!presenter_start(av.geometry.base_width, av.geometry.base_height)) {
        runner_log("out of memory");
        fb_close();
        core_stop(rom_data);
        return 1;
    }
    input_open();
    input_pump();
    if (active_contacts() > 0) gesture.ignore = true; /* the touch that started the game is not a tap */
    bool have_audio = audio_open((unsigned) (av.timing.sample_rate + 0.5));
    start_watchdog();

    uint64_t frame_ns = (uint64_t) (1e9 / av.timing.fps), next = now_ns(), late = 0, frames = 0;
    uint64_t run_total_ns = 0, audio_total_ns = 0, sleep_total_ns = 0, run_max_ns = 0;
    double c_start = cpu_seconds();
    uint64_t t_start = now_ns();
    uint64_t stats_time = t_start, stats_frames = 0, stats_core_ns = 0, stats_scale_ns = video_scaling_ns();
    uint64_t stats_audio_ns = 0;
    double stats_cpu_s = c_start;
    unsigned stats_underruns = audio_underrun_count();
    while (!quit_flag) {
        atomic_fetch_add(&heartbeat, 1);
        input_pump();
        if (quit_flag) break;
        if (menu_open) {
            menu_step();
            if (stats_interval) {
                stats_time = now_ns();
                stats_frames = frames;
                stats_core_ns = run_total_ns;
                stats_scale_ns = video_scaling_ns();
                stats_audio_ns = audio_total_ns;
                stats_cpu_s = cpu_seconds();
                stats_underruns = audio_underrun_count();
            }
            continue;
        }
        if (resume_pending) {
            resume_pending = false;
            next = now_ns();
        }
        bool open_menu = power_tap || (gesture_result.done && tap_in_game_center(&gesture_result));
        power_tap = false;
        gesture_result.done = false;
        if (open_menu) {
            menu_begin();
            continue;
        }
        frameskip_begin();
        uint64_t run_t0 = now_ns();
        retro_run();
        uint64_t run_t1 = now_ns();
        uint64_t run_ns = run_t1 - run_t0;
        bool audio_paced = audio_flush();
        run_total_ns += run_ns;
        audio_total_ns += now_ns() - run_t1;
        if (run_ns > run_max_ns) run_max_ns = run_ns;
        frameskip_end(run_ns, frame_ns);
        frames++;
        if (av_pending) {
            /* The core changed its timing or sample rate while running. */
            struct retro_system_av_info now_av;
            av_pending = false;
            retro_get_system_av_info(&now_av);
            if (now_av.timing.fps > 0) frame_ns = (uint64_t) (1e9 / now_av.timing.fps);
            if ((unsigned) (now_av.timing.sample_rate + 0.5) != (unsigned) (av.timing.sample_rate + 0.5)) {
                audio_close();
                have_audio = audio_open((unsigned) (now_av.timing.sample_rate + 0.5));
                if (stats_interval) {
                    stats_time = now_ns();
                    stats_frames = frames;
                    stats_core_ns = run_total_ns;
                    stats_scale_ns = video_scaling_ns();
                    stats_audio_ns = audio_total_ns;
                    stats_cpu_s = cpu_seconds();
                    stats_underruns = audio_underrun_count();
                }
            }
            av = now_av;
        }
        next += frame_ns;
        uint64_t now = now_ns();
        if (have_audio && audio_paced) {
            next = now;
        } else if (now < next) {
            struct timespec ts = { (time_t) ((next - now) / 1000000000ull), (long) ((next - now) % 1000000000ull) };
            nanosleep(&ts, NULL);
            sleep_total_ns += next - now;
        } else if (now - next > frame_ns) {
            late++;
            next = now;
        }
        if (stats_interval)
            interactive_stats((uint64_t) stats_interval * 1000000000ull, &stats_time, &stats_frames,
                              &stats_core_ns, &stats_scale_ns, &stats_audio_ns, &stats_cpu_s,
                              &stats_underruns, frames, run_total_ns, audio_total_ns,
                              av.timing.fps);
    }
    double wall = (double) (now_ns() - t_start) / 1e9;
    runner_log("ran %llu frames in %.1fs (%.1f fps), cpu %.0f%%, late resyncs %llu, max touches %d",
               (unsigned long long) frames, wall, wall > 0 ? (double) frames / wall : 0.0,
               wall > 0 ? 100 * (cpu_seconds() - c_start) / wall : 0.0, (unsigned long long) late, max_touches);
    if (frames) {
        const double n = (double) frames, ms = 1e6;
        runner_log("per frame: core %.2f ms (max %.2f; video scaling %.2f ms on presenter), audio write %.2f ms (blocked "
                   "pacing), sleep %.2f ms, budget %.2f ms; core alone %.0f%% of real time",
                   (double) run_total_ns / ms / n, (double) run_max_ns / ms, (double) video_scaling_ns() / ms / n,
                   (double) audio_total_ns / ms / n, (double) sleep_total_ns / ms / n, (double) frame_ns / ms,
                   run_total_ns ? 100.0 * n * (double) frame_ns / (double) run_total_ns : 0.0);
    }
    if (cfg.frameskip != FRAMESKIP_OFF)
        runner_log("frameskip: %llu of %llu frames had their video skipped", (unsigned long long) video_skipped,
                   (unsigned long long) frames);

    wait_for_release();
    sram_save(rom);
    audio_close();
    input_close();
    presenter_stop();
    if (retired_presenters.submitted) {
        const double shown = retired_presenters.shown ? (double) retired_presenters.shown : 1.0, ms = 1e6;
        runner_log("present: %llu frames handed over, %llu shown, %llu dropped (a newer one replaced them); "
                   "show avg %.2f max %.2f ms = copy avg %.2f + pan avg %.2f ms (pan max %.2f ms, the wait for the display)",
                   (unsigned long long) retired_presenters.submitted, (unsigned long long) retired_presenters.shown,
                   (unsigned long long) retired_presenters.dropped, (double) retired_presenters.show_ns_total / ms / shown,
                   (double) retired_presenters.show_ns_max / ms, (double) copy_ns_total / ms / shown,
                   (double) pan_ns_total / ms / shown, (double) pan_ns_max / ms);
    }
    fb_close();
    free(shadow);
    shadow = NULL;
    core_stop(rom_data);
    return 0;
}

/* Applies command line overrides over the emulator.json settings. */
static bool apply_cli_overrides(char ** opts, int opt_count, char ** skip_opts, int skip_opt_count,
                                const char * frameskip, const char * stretch_arg,
                                const char * audio_buffer_arg, const char * residency_arg) {
    if (stretch_arg) {
        if (strcmp(stretch_arg, "on") != 0 && strcmp(stretch_arg, "off") != 0) {
            fprintf(stderr, LOG_PREFIX "--audio-stretch needs on or off, got %s\n", stretch_arg);
            return false;
        }
        cfg.audio_stretch = strcmp(stretch_arg, "on") == 0;
    }
    if (audio_buffer_arg && !frontend_config_parse_audio_buffer_ms(audio_buffer_arg, &cfg.audio_buffer_ms)) {
        fprintf(stderr, LOG_PREFIX "--audio-buffer-ms needs an integer from 0 to 2000, got %s\n", audio_buffer_arg);
        return false;
    }
    if (residency_arg && !frontend_config_parse_code_residency(residency_arg, &cfg.code_residency)) {
        fprintf(stderr, LOG_PREFIX "--code-residency needs on or off, got %s\n", residency_arg);
        return false;
    }
    if (frameskip && !frontend_config_parse_frameskip(frameskip, &cfg.frameskip)) {
        fprintf(stderr, LOG_PREFIX "unknown frameskip mode %s\n", frameskip);
        return false;
    }
    for (int i = 0; i < opt_count + skip_opt_count; i++) {
        bool skip = i >= opt_count;
        const char * arg = skip ? skip_opts[i - opt_count] : opts[i];
        char key[FRONTEND_OPTION_TEXT];
        const char * eq = strchr(arg, '=');
        size_t key_len = eq ? (size_t) (eq - arg) : 0;
        if (!eq || !key_len || key_len >= sizeof(key)) {
            fprintf(stderr, LOG_PREFIX "--opt and --skip-opt need key=value, got %s\n", arg);
            return false;
        }
        memcpy(key, arg, key_len);
        key[key_len] = '\0';
        if (!frontend_config_set_option(&cfg, key, eq + 1, skip)) {
            fprintf(stderr, LOG_PREFIX "too many options\n");
            return false;
        }
    }
    return true;
}

int frontend_main(int argc, char ** argv) {
    install_signal_handlers();
    if (argc < 2) {
        fprintf(stderr, LOG_PREFIX "usage: %s ROM [--bench N] [--opt key=value]... [--frameskip off|auto|half] [--audio-stretch on|off] [--audio-buffer-ms N] [--code-residency on|off] "
                "[--stats-interval N] [--skip-opt key=value]... [--budget-ms N]\n", argv[0]);
        return 2;
    }
    const char * rom = argv[1];
    const char * frameskip = NULL, * stretch_arg = NULL, * audio_buffer_arg = NULL, * residency_arg = NULL;
    char * opts[FRONTEND_MAX_OPTIONS], * skip_opts[FRONTEND_MAX_OPTIONS];
    int opt_count = 0, skip_opt_count = 0;
    long bench = 0;
    unsigned stats_interval = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            bench = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--budget-ms") == 0 && i + 1 < argc) {
            bench_budget_ns = (uint64_t) (strtod(argv[++i], NULL) * 1e6);
        } else if (strcmp(argv[i], "--frameskip") == 0 && i + 1 < argc) {
            frameskip = argv[++i];
        } else if (strcmp(argv[i], "--audio-stretch") == 0 && i + 1 < argc) {
            stretch_arg = argv[++i];
        } else if (strcmp(argv[i], "--audio-buffer-ms") == 0 && i + 1 < argc) {
            audio_buffer_arg = argv[++i];
        } else if (strcmp(argv[i], "--code-residency") == 0 && i + 1 < argc) {
            residency_arg = argv[++i];
        } else if (strcmp(argv[i], "--stats-interval") == 0 && i + 1 < argc) {
            char * end = NULL;
            errno = 0;
            long seconds = strtol(argv[++i], &end, 10);
            if (errno || !end || *end || seconds <= 0 || (unsigned long) seconds > UINT_MAX) {
                fprintf(stderr, LOG_PREFIX "--stats-interval needs a positive number of seconds\n");
                return 2;
            }
            stats_interval = (unsigned) seconds;
        } else if (strcmp(argv[i], "--opt") == 0 && i + 1 < argc && opt_count < FRONTEND_MAX_OPTIONS) {
            opts[opt_count++] = argv[++i];
        } else if (strcmp(argv[i], "--skip-opt") == 0 && i + 1 < argc && skip_opt_count < FRONTEND_MAX_OPTIONS) {
            skip_opts[skip_opt_count++] = argv[++i];
        } else {
            fprintf(stderr, LOG_PREFIX "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    load_settings(argv[0]);
    if (!apply_cli_overrides(opts, opt_count, skip_opts, skip_opt_count, frameskip, stretch_arg,
                             audio_buffer_arg, residency_arg)) return 2;
#ifndef HOST_BUILD
    if (cfg.code_residency) {
        extern char etext[];
        (void) residency_pin_code((const void *) frontend_main, etext);
    }
#endif
    return bench > 0 ? run_bench(rom, bench) : run_interactive(rom, stats_interval);
}
