/* Shared libretro frontend for Compas emulator packages (protocol 1, see
 * docs/PROTOCOL.md). One copy serves every emulator: the core is linked
 * statically and EMULATOR_ID names the program in its log lines.
 *
 * It maps the framebuffer, grabs every input device so the player underneath
 * sees no events, draws the game at the top of the screen with touch
 * controls below it, and paces itself on blocking audio writes. It exits on
 * the on-screen EXIT box, a one second power button hold, or SIGTERM. */
#include "frontend.h"

#include "glyph5x7.h"
#include "libretro.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
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
#define MAX_INPUT_DEVICES 8
#define MAX_CONTACTS 10
#define POWER_HOLD_QUIT_NS 1000000000ull
#define RELEASE_WAIT_NS 2000000000ull
#define WATCHDOG_STALL_SECONDS 5
#define TOUCH_TRACE_LINES 40

static volatile sig_atomic_t quit_flag;
static atomic_uint heartbeat; /* advanced by every pass of the main loop */
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static char save_dir[512] = ".";

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
static int max_game_h;               /* rows available above the controls */
static int game_h;                   /* scaled game height */
static int xmap[MAX_SURFACE_WIDTH], ymap[MAX_SURFACE_WIDTH];
static int map_src_w, map_src_h;
static unsigned last_src_w, last_src_h;
static uint64_t video_frames;
static uint64_t scale_ns_total;

static void flip_page(void);

static void build_maps(int src_w, int src_h) {
    map_src_w = src_w;
    map_src_h = src_h;
    game_h = src_h * surf_w / src_w;
    if (game_h > max_game_h) game_h = max_game_h;
    for (int x = 0; x < surf_w; x++) xmap[x] = x * src_w / surf_w;
    for (int y = 0; y < game_h; y++) ymap[y] = y * src_h / game_h;
}

static inline uint16_t pixel_to_565(const void * row, int x) {
    if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) return ((const uint16_t *) row)[x];
    if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
        uint32_t p = ((const uint32_t *) row)[x];
        return (uint16_t) (((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
    }
    uint16_t p = ((const uint16_t *) row)[x];
    return (uint16_t) (((p & 0x7FE0) << 1) | (p & 0x1F));
}

/* Framebuffer memory is uncached: never read it back, and write whole rows
 * with memcpy from a row composed in normal RAM. */
static void video_cb(const void * data, unsigned w, unsigned h, size_t pitch) {
    static uint16_t line[MAX_SURFACE_WIDTH];
    if (!data || !surf || !w || !h) return;
    uint64_t t0 = now_ns();
    last_src_w = w;
    last_src_h = h;
    if ((int) w != map_src_w || (int) h != map_src_h) build_maps((int) w, (int) h);
    const uint8_t * src = data;
    int last_sy = -1;
    for (int y = 0; y < game_h; y++) {
        int sy = ymap[y];
        if (sy != last_sy) {
            const void * row = src + (size_t) sy * pitch;
            if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
                const uint16_t * r = row;
                for (int x = 0; x < surf_w; x++) line[x] = r[xmap[x]];
            } else {
                for (int x = 0; x < surf_w; x++) line[x] = pixel_to_565(row, xmap[x]);
            }
            last_sy = sy;
        }
        memcpy(surf + (size_t) y * surf_stride, line, (size_t) surf_w * 2);
    }
    scale_ns_total += now_ns() - t0;
    video_frames++;
    flip_page();
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
/* The R1 codec only accepts four periods; 512-frame periods underrun
 * constantly (see the comment in audio_output.c). Blocking writes against
 * this buffer are what pace the emulation. */
#define AUDIO_PERIOD_FRAMES 1024
#define AUDIO_PERIOD_COUNT 4
/* The player's audio thread closes its device asynchronously after a pause. */
#define AUDIO_OPEN_RETRY_MS 100
#define AUDIO_OPEN_TIMEOUT_MS 2000

static struct pcm * pcm;
static unsigned audio_errors;

static bool audio_open(unsigned rate) {
    struct pcm_config cfg = { 0 };
    cfg.channels = 2;
    cfg.rate = rate;
    cfg.period_size = AUDIO_PERIOD_FRAMES;
    cfg.period_count = AUDIO_PERIOD_COUNT;
    cfg.format = PCM_FORMAT_S16_LE;
    cfg.start_threshold = cfg.period_size * cfg.period_count;
    cfg.stop_threshold = cfg.period_size * cfg.period_count;
    for (int waited_ms = 0;; waited_ms += AUDIO_OPEN_RETRY_MS) {
        pcm = pcm_open(0, 0, PCM_OUT, &cfg);
        if (pcm && pcm_is_ready(pcm)) return true;
        const char * why = pcm ? pcm_get_error(pcm) : "open failed";
        if (waited_ms >= AUDIO_OPEN_TIMEOUT_MS) {
            runner_log("audio: %s (continuing silent)", why);
            if (pcm) pcm_close(pcm);
            pcm = NULL;
            return false;
        }
        if (pcm) pcm_close(pcm);
        pcm = NULL;
        usleep(AUDIO_OPEN_RETRY_MS * 1000);
    }
}

/* False when the write failed, so the caller paces the frame itself. */
static bool audio_flush(void) {
    bool ok = pcm != NULL;
    if (pcm && audio_buf_frames && pcm_writei(pcm, audio_buf, (unsigned) audio_buf_frames) < 0) {
        audio_errors++;
        pcm_prepare(pcm);
        ok = false;
    }
    audio_buf_frames = 0;
    return ok;
}

static void audio_close(void) {
    if (pcm) pcm_close(pcm);
    pcm = NULL;
    if (audio_errors) runner_log("audio write errors: %u", audio_errors);
}
#else
static bool audio_open(unsigned rate) { (void) rate; return false; }
static bool audio_flush(void) { audio_buf_frames = 0; return false; }
static void audio_close(void) {}
#endif

/* ---- framebuffer ---- */

static int fb_fd = -1;
static uint8_t * fb_mem;
static size_t fb_len;
static struct fb_var_screeninfo vinfo, vinfo_orig;
static int fb_page;                  /* page being drawn into */
static int fb_pages;

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
    if (vinfo.bits_per_pixel != 16 || vinfo.xres > MAX_SURFACE_WIDTH) {
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
    surf = fb_page_ptr(fb_page);
    return true;
}

/* Shows the page just drawn and starts drawing into the other one. */
static void flip_page(void) {
    if (fb_fd < 0 || fb_pages < 2) return;
    vinfo.yoffset = (unsigned) (fb_page * vinfo.yres);
    ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
    fb_page ^= 1;
    surf = fb_page_ptr(fb_page);
}

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
 * to the control area. */

#define COLOR_PANEL 0x18E3
#define COLOR_DPAD 0x39E7
#define COLOR_DPAD_ARM 0x6B4D
#define COLOR_BUTTON_AB 0xA145
#define COLOR_BUTTON_META 0x6B4D
#define COLOR_EXIT 0x8000
#define COLOR_LABEL 0xFFFF
#define COLOR_LABEL_DIM 0xC618

typedef struct {
    int cx, cy, r;
    unsigned bit;
    const char * label;
} pad_button_t;

static struct {
    int top, w, h;                   /* control area on the screen */
    int dpad_cx, dpad_cy, dpad_r, dpad_arm, dpad_dead, dpad_reach;
    pad_button_t buttons[4];         /* A, B, SELECT, START */
    int button_reach, ab_scale, meta_scale, meta_label_dy;
    int exit_x0, exit_y0, exit_x1, exit_y1;
} lay;

static void layout_controls(int top) {
    lay.top = top;
    lay.w = surf_w;
    lay.h = surf_h - top;
    int m = lay.w < lay.h ? lay.w : lay.h;
    lay.dpad_r = m * 28 / 100;
    lay.dpad_cx = lay.w / 4;
    lay.dpad_cy = lay.h * 46 / 100;
    lay.dpad_arm = lay.dpad_r * 29 / 100;
    lay.dpad_dead = m * 5 / 100;
    lay.dpad_reach = lay.dpad_r + m * 8 / 100;
    int ab_r = m * 13 / 100;
    int meta_r = m * 85 / 1000;
    lay.button_reach = m * 4 / 100;
    lay.ab_scale = ab_r / 8 > 1 ? ab_r / 8 : 1;
    lay.meta_scale = (m + 50) / 150;
    lay.meta_label_dy = meta_r + m * 4 / 100 + 7 * lay.meta_scale / 2;
    int meta_cy = lay.h - meta_r - m * 5 / 100;
    lay.buttons[0] = (pad_button_t) { lay.w * 855 / 1000, lay.dpad_cy - m * 10 / 100, ab_r, RETRO_DEVICE_ID_JOYPAD_A, "A" };
    lay.buttons[1] = (pad_button_t) { lay.w * 625 / 1000, lay.dpad_cy + m * 10 / 100, ab_r, RETRO_DEVICE_ID_JOYPAD_B, "B" };
    lay.buttons[2] = (pad_button_t) { lay.w / 2 - lay.w * 104 / 1000, meta_cy, meta_r, RETRO_DEVICE_ID_JOYPAD_SELECT, "SELECT" };
    lay.buttons[3] = (pad_button_t) { lay.w / 2 + lay.w * 104 / 1000, meta_cy, meta_r, RETRO_DEVICE_ID_JOYPAD_START, "START" };
    int exit_size = m * 17 / 100, gap = m * 12 / 1000 + 2;
    lay.exit_x1 = lay.w - gap;
    lay.exit_x0 = lay.exit_x1 - exit_size;
    lay.exit_y0 = gap;
    lay.exit_y1 = gap + exit_size;
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
    for (int i = 0; i < 4; i++)
        fill_circle(c, lay.buttons[i].cx, lay.buttons[i].cy, lay.buttons[i].r, i < 2 ? COLOR_BUTTON_AB : COLOR_BUTTON_META);
    fill_rect(c, lay.exit_x0, lay.exit_y0, lay.exit_x1, lay.exit_y1, COLOR_EXIT);
    for (int i = 0; i < 2; i++)
        draw_text(c, lay.buttons[i].cx, lay.buttons[i].cy, lay.buttons[i].label, lay.ab_scale, COLOR_LABEL);
    for (int i = 2; i < 4; i++)
        draw_text(c, lay.buttons[i].cx, lay.buttons[i].cy - lay.meta_label_dy, lay.buttons[i].label,
                  lay.meta_scale, COLOR_LABEL_DIM);
    draw_text(c, (lay.exit_x0 + lay.exit_x1) / 2, (lay.exit_y0 + lay.exit_y1) / 2, "EXIT", lay.meta_scale, COLOR_LABEL);
}

/* Paints both pages: black above the game's rows, the controls below. The
 * controls are composed once in RAM and copied a row at a time. */
static bool draw_screen(void) {
    canvas_t c = { malloc((size_t) lay.w * lay.h * 2), lay.w, lay.h };
    uint16_t * black = calloc((size_t) surf_w, 2);
    if (!c.px || !black) {
        free(c.px);
        free(black);
        return false;
    }
    render_controls(&c);
    for (int page = 0; page < fb_pages; page++) {
        uint16_t * p = fb_page_ptr(page);
        for (int y = 0; y < lay.top; y++) memcpy(p + (size_t) y * surf_stride, black, (size_t) surf_w * 2);
        for (int y = 0; y < lay.h; y++)
            memcpy(p + (size_t) (lay.top + y) * surf_stride, c.px + (size_t) y * c.w, (size_t) c.w * 2);
    }
    free(c.px);
    free(black);
    return true;
}

/* Joypad bits pressed by one contact at screen position (x, y). */
static uint16_t touch_bits(int x, int y) {
    y -= lay.top;
    if (y < 0) return 0;
    if (x >= lay.exit_x0 && x < lay.exit_x1 && y >= lay.exit_y0 && y < lay.exit_y1) {
        quit_flag = 1;
        return 0;
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
    for (int i = 0; i < 4; i++) {
        int bx = x - lay.buttons[i].cx, by = y - lay.buttons[i].cy, r = lay.buttons[i].r + lay.button_reach;
        if (bx * bx + by * by <= r * r) return (uint16_t) (1u << lay.buttons[i].bit);
    }
    return 0;
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

static int key_to_pad_bit(int code) {
    switch (code) {
        case KEY_VOLUMEUP: return RETRO_DEVICE_ID_JOYPAD_A;
        case KEY_VOLUMEDOWN: return RETRO_DEVICE_ID_JOYPAD_B;
        case KEY_PLAYPAUSE: return RETRO_DEVICE_ID_JOYPAD_START;
        case KEY_PREVIOUSSONG: return RETRO_DEVICE_ID_JOYPAD_SELECT;
        case KEY_NEXTSONG: return RETRO_DEVICE_ID_JOYPAD_A;
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
    uint16_t bits = key_pad_bits;
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
        if (ev->value == 1) power_down_ns = now_ns();
        else if (ev->value == 0) power_down_ns = 0;
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

/* Drains every device and refreshes pad_bits; requests quit on a power hold. */
static void input_pump(void) {
    if (power_down_ns && now_ns() - power_down_ns >= POWER_HOLD_QUIT_NS) quit_flag = 1;
    struct pollfd pfds[MAX_INPUT_DEVICES];
    for (int i = 0; i < dev_count; i++) pfds[i] = (struct pollfd) { devs[i].fd, POLLIN, 0 };
    expire_type_a_contacts();
    if (!dev_count || poll(pfds, (nfds_t) dev_count, 0) <= 0) {
        recompute_pad();
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
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **) data = save_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((struct retro_log_callback *) data)->log = core_log;
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool *) data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool *) data = false;
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
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

static void sram_path(const char * rom, char * out, size_t size) {
    snprintf(out, size, "%s", rom);
    char * slash = strrchr(out, '/');
    char * dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    size_t len = strlen(out);
    snprintf(out + len, size - len, ".srm");
}

static void sram_load(const char * rom) {
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void * mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!n || !mem) return;
    char path[640];
    sram_path(rom, path, sizeof(path));
    FILE * f = fopen(path, "rb");
    if (!f) return;
    if (fread(mem, 1, n, f) != n) runner_log("sram: short read from %s", path);
    fclose(f);
}

/* Temp file plus rename, so a power loss never leaves a truncated save. */
static void sram_save(const char * rom) {
    size_t n = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    const void * mem = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!n || !mem) return;
    char path[640], tmp[648];
    sram_path(rom, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "wb");
    if (!f) {
        runner_log("sram: cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    bool ok = fwrite(mem, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        runner_log("sram: saving %s failed", path);
        unlink(tmp);
    }
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

bool frontend_bench(const char * rom, long frames, frontend_bench_t * out) {
    memset(out, 0, sizeof(*out));
    set_save_dir(rom);
    core_start();
    struct retro_system_av_info av;
    void * rom_data;
    if (!core_load(rom, &av, &rom_data)) {
        retro_deinit();
        return false;
    }
    surf_w = 480;
    surf_h = 800;
    surf_stride = 480;
    max_game_h = surf_h;
    surf = calloc((size_t) surf_stride * surf_h, 2);
    uint64_t t_start = now_ns();
    double c_start = cpu_seconds();
    for (long f = 0; f < frames; f++) {
        pad_bits = bench_input((uint64_t) f);
        retro_run();
        audio_flush();
    }
    out->wall_s = (double) (now_ns() - t_start) / 1e9;
    out->cpu_s = cpu_seconds() - c_start;
    out->width = last_src_w;
    out->height = last_src_h;
    out->scaled_width = surf_w;
    out->scaled_height = game_h;
    out->sample_rate = av.timing.sample_rate;
    out->fps = av.timing.fps;
    out->frames = (uint64_t) frames;
    out->video_frames = video_frames;
    out->audio_frames = audio_frames_total;
    out->vm_hwm_kb = status_kb("VmHWM");
    out->sram_size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
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
    return 0;
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

static int run_interactive(const char * rom) {
    set_save_dir(rom);
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
    max_game_h = surf_h;
    build_maps((int) av.geometry.base_width, (int) av.geometry.base_height);
    max_game_h = game_h;
    layout_controls(game_h);
    if (!draw_screen()) {
        runner_log("out of memory");
        fb_close();
        core_stop(rom_data);
        return 1;
    }
    input_open();
    bool have_audio = audio_open((unsigned) (av.timing.sample_rate + 0.5));
    start_watchdog();

    uint64_t frame_ns = (uint64_t) (1e9 / av.timing.fps), next = now_ns(), late = 0, frames = 0;
    double c_start = cpu_seconds();
    uint64_t t_start = now_ns();
    while (!quit_flag) {
        atomic_fetch_add(&heartbeat, 1);
        input_pump();
        if (quit_flag) break;
        retro_run();
        bool audio_paced = audio_flush();
        frames++;
        next += frame_ns;
        uint64_t now = now_ns();
        if (have_audio && audio_paced) {
            next = now;
        } else if (now < next) {
            struct timespec ts = { (time_t) ((next - now) / 1000000000ull), (long) ((next - now) % 1000000000ull) };
            nanosleep(&ts, NULL);
        } else if (now - next > frame_ns) {
            late++;
            next = now;
        }
    }
    double wall = (double) (now_ns() - t_start) / 1e9;
    runner_log("ran %llu frames in %.1fs (%.1f fps), cpu %.0f%%, late resyncs %llu, max touches %d",
               (unsigned long long) frames, wall, wall > 0 ? (double) frames / wall : 0.0,
               wall > 0 ? 100 * (cpu_seconds() - c_start) / wall : 0.0, (unsigned long long) late, max_touches);

    wait_for_release();
    sram_save(rom);
    audio_close();
    input_close();
    fb_close();
    core_stop(rom_data);
    return 0;
}

int frontend_main(int argc, char ** argv) {
    install_signal_handlers();
    if (argc < 2) {
        fprintf(stderr, LOG_PREFIX "usage: %s ROM [--bench N]\n", argv[0]);
        return 2;
    }
    const char * rom = argv[1];
    long bench = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) bench = strtol(argv[++i], NULL, 10);
        else {
            fprintf(stderr, LOG_PREFIX "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    return bench > 0 ? run_bench(rom, bench) : run_interactive(rom);
}
