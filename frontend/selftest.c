/* Host self-test for the frontend: builds a minimal iNES ROM, runs it
 * through the headless benchmark path and checks the frontend's video and
 * audio plumbing against the statically linked QuickNES core. */
#include "config.h"
#include "audio_policy.h"
#include "frontend.h"
#include "libretro.h"
#include "present.h"
#include "stretch.h"
#include "xrun.h"
#include "video_scale.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PRG_SIZE 0x4000
#define CHR_SIZE 0x2000
#define FRAMES 600

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* Mapper 0, 16 KB PRG, 8 KB CHR, battery-backed RAM. PRG is one infinite
 * loop (SEI, CLD, LDX #$FF, TXS, JMP self) with reset, NMI and IRQ vectors
 * all pointing at it. */
static bool write_test_rom(const char * path) {
    static uint8_t rom[16 + PRG_SIZE + CHR_SIZE];
    memset(rom, 0, sizeof(rom));
    memcpy(rom, "NES\x1a", 4);
    rom[4] = 1;
    rom[5] = 1;
    rom[6] = 0x02;
    static const uint8_t code[] = { 0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0x4C, 0x05, 0xC0 };
    memcpy(rom + 16, code, sizeof(code));
    uint8_t * vectors = rom + 16 + PRG_SIZE - 6;
    for (int i = 0; i < 3; i++) {
        vectors[i * 2] = 0x00;
        vectors[i * 2 + 1] = 0xC0;
    }
    FILE * f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(rom, 1, sizeof(rom), f) == sizeof(rom);
    return fclose(f) == 0 && ok;
}

#define BTN(id) (1u << (id))

static void check_config(void) {
    frontend_config_t c;
    char why[64];
    frontend_config_default(&c);
    CHECK(c.audio_buffer_ms == 46, "default audio buffer is %u ms", c.audio_buffer_ms);
    CHECK(c.code_residency, "code residency is not enabled by default");
    CHECK(c.game_height_percent == 68, "default viewport height is %u percent", c.game_height_percent);
    static const unsigned heights[] = {40, 55, 80};
    for (size_t i = 0; i < sizeof(heights) / sizeof(heights[0]); ++i) {
        char json[80];
        snprintf(json, sizeof(json), "{\"frontend\":{\"game_height_percent\":%u}}", heights[i]);
        CHECK(frontend_config_parse(json, strlen(json), &c, why, sizeof(why)) && c.game_height_percent == heights[i],
              "viewport height %u rejected: %s", heights[i], why);
    }
    static const char * const height_bad[] = { "39", "81", "-1", "55.5", "5e1", "\"55\"", "true", "null", "[]",
                                              "999999999999999999999999" };
    for (size_t i = 0; i < sizeof(height_bad) / sizeof(height_bad[0]); ++i) {
        char json[96];
        snprintf(json, sizeof(json), "{\"frontend\":{\"game_height_percent\":%s}}", height_bad[i]);
        CHECK(!frontend_config_parse(json, strlen(json), &c, why, sizeof(why)) && c.game_height_percent == 68,
              "invalid viewport height %s accepted or did not restore default", height_bad[i]);
    }
    CHECK(c.buttons == (BTN(RETRO_DEVICE_ID_JOYPAD_A) | BTN(RETRO_DEVICE_ID_JOYPAD_B) | BTN(RETRO_DEVICE_ID_JOYPAD_SELECT) |
                        BTN(RETRO_DEVICE_ID_JOYPAD_START)), "default buttons are not the NES set");

    static const char good[] =
        "{ \"id\": \"x\", \"systems\": [ { \"a\": [1, {\"b\": null}] } ], \"description\": \"q\\\"uote\",\n"
        "  \"frontend\": { \"buttons\": [\"b\", \"a\", \"l\", \"r\", \"select\", \"start\"], \"extra\": [true, 1.5e3],\n"
        "    \"options\": { \"gpsp_drc\": \"enabled\", \"n\": 3, \"flag\": true } }, \"last\": 0 }";
    CHECK(frontend_config_parse(good, sizeof(good) - 1, &c, why, sizeof(why)), "valid manifest rejected: %s", why);
    CHECK(c.buttons == (BTN(RETRO_DEVICE_ID_JOYPAD_A) | BTN(RETRO_DEVICE_ID_JOYPAD_B) | BTN(RETRO_DEVICE_ID_JOYPAD_L) |
                        BTN(RETRO_DEVICE_ID_JOYPAD_R) | BTN(RETRO_DEVICE_ID_JOYPAD_SELECT) | BTN(RETRO_DEVICE_ID_JOYPAD_START)),
          "buttons 0x%x", c.buttons);
    CHECK(c.option_count == 3, "option count %d", c.option_count);
    const char * v = frontend_config_option(&c, "gpsp_drc");
    CHECK(v && strcmp(v, "enabled") == 0, "gpsp_drc option");
    v = frontend_config_option(&c, "n");
    CHECK(v && strcmp(v, "3") == 0, "numeric option");
    v = frontend_config_option(&c, "flag");
    CHECK(v && strcmp(v, "true") == 0, "boolean option");
    CHECK(!frontend_config_option(&c, "missing"), "unknown option answered");
    CHECK(c.frameskip == FRAMESKIP_OFF && c.skip_option_count == 0, "frameskip is not off by default");

    static const char skip[] =
        "{ \"frontend\": { \"frameskip\": \"auto\", \"options\": { \"a\": 1 }, \"skip_options\": { \"a\": 2, \"b\": \"x\" } } }";
    CHECK(frontend_config_parse(skip, sizeof(skip) - 1, &c, why, sizeof(why)), "frameskip manifest rejected: %s", why);
    CHECK(c.frameskip == FRAMESKIP_AUTO && c.skip_option_count == 2, "frameskip %d, %d skip options", c.frameskip, c.skip_option_count);
    v = frontend_config_skip_option(&c, "a");
    CHECK(v && strcmp(v, "2") == 0 && strcmp(frontend_config_option(&c, "a"), "1") == 0, "skip option does not replace the normal one");
    CHECK(!frontend_config_skip_option(&c, "missing"), "unknown skip option answered");
    CHECK(frontend_config_set_option(&c, "a", "9", true) && strcmp(frontend_config_skip_option(&c, "a"), "9") == 0 &&
          c.skip_option_count == 2, "skip option not replaced in place");
    frameskip_mode_t mode;
    CHECK(frontend_config_parse_frameskip("half", &mode) && mode == FRAMESKIP_HALF, "half frameskip");
    CHECK(!frontend_config_parse_frameskip("fast", &mode), "unknown frameskip accepted");
    static const char stretch_on[] = "{\"frontend\":{\"audio_stretch\":true}}", stretch_off[] = "{\"frontend\":{\"audio_stretch\":false}}";
    CHECK(frontend_config_parse(stretch_on, sizeof(stretch_on) - 1, &c, why, sizeof(why)) && c.audio_stretch, "audio_stretch true");
    CHECK(frontend_config_parse(stretch_off, sizeof(stretch_off) - 1, &c, why, sizeof(why)) && !c.audio_stretch, "audio_stretch false");
    CHECK(frontend_config_parse("{}", 2, &c, why, sizeof(why)) && !c.audio_stretch, "audio_stretch default");
    CHECK(c.code_residency, "code_residency default");
    static const char residency_on[] = "{\"frontend\":{\"code_residency\":true}}";
    static const char residency_off[] = "{\"frontend\":{\"code_residency\":false}}";
    CHECK(frontend_config_parse(residency_on, sizeof(residency_on) - 1, &c, why, sizeof(why)) && c.code_residency,
          "code_residency true: %s", why);
    CHECK(frontend_config_parse(residency_off, sizeof(residency_off) - 1, &c, why, sizeof(why)) && !c.code_residency,
          "code_residency false: %s", why);
    static const char * const residency_bad[] = {
        "{\"frontend\":{\"code_residency\":\"true\"}}", "{\"frontend\":{\"code_residency\":1}}",
        "{\"frontend\":{\"code_residency\":null}}",
    };
    for (size_t i = 0; i < sizeof(residency_bad) / sizeof(residency_bad[0]); ++i)
        CHECK(!frontend_config_parse(residency_bad[i], strlen(residency_bad[i]), &c, why, sizeof(why)),
              "accepted invalid code_residency %zu", i);
    bool residency_enabled = false;
    CHECK(frontend_config_parse_code_residency("on", &residency_enabled) && residency_enabled,
          "CLI code residency on");
    CHECK(frontend_config_parse_code_residency("off", &residency_enabled) && !residency_enabled,
          "CLI code residency off");
    CHECK(!frontend_config_parse_code_residency("true", &residency_enabled) &&
          !frontend_config_parse_code_residency("bad", &residency_enabled), "CLI accepted invalid code residency");
    CHECK(c.audio_buffer_ms == 46, "audio buffer default is %u ms", c.audio_buffer_ms);
    static const char audio_short[] = "{\"frontend\":{\"audio_buffer_ms\":46}}";
    static const char audio_safe[] = "{\"frontend\":{\"audio_buffer_ms\":0}}";
    static const char audio_max[] = "{\"frontend\":{\"audio_buffer_ms\":2000}}";
    CHECK(frontend_config_parse(audio_short, sizeof(audio_short) - 1, &c, why, sizeof(why)) && c.audio_buffer_ms == 46,
          "audio_buffer_ms short value: %s", why);
    CHECK(frontend_config_parse(audio_safe, sizeof(audio_safe) - 1, &c, why, sizeof(why)) && c.audio_buffer_ms == 0,
          "audio_buffer_ms safe value: %s", why);
    CHECK(frontend_config_parse(audio_max, sizeof(audio_max) - 1, &c, why, sizeof(why)) && c.audio_buffer_ms == 2000,
          "audio_buffer_ms maximum: %s", why);
    uint32_t parsed_ms;
    CHECK(frontend_config_parse_audio_buffer_ms("00046", &parsed_ms) && parsed_ms == 46,
          "CLI audio buffer parser rejected decimal value");
    static const char * const audio_bad[] = {
        "{\"frontend\":{\"audio_buffer_ms\":-1}}", "{\"frontend\":{\"audio_buffer_ms\":2001}}",
        "{\"frontend\":{\"audio_buffer_ms\":46.5}}", "{\"frontend\":{\"audio_buffer_ms\":\"46\"}}",
        "{\"frontend\":{\"audio_buffer_ms\":true}}", "{\"frontend\":{\"audio_buffer_ms\":1e2}}",
    };
    for (size_t i = 0; i < sizeof(audio_bad) / sizeof(audio_bad[0]); ++i)
        CHECK(!frontend_config_parse(audio_bad[i], strlen(audio_bad[i]), &c, why, sizeof(why)),
              "accepted invalid audio_buffer_ms %zu", i);
    CHECK(!frontend_config_parse_audio_buffer_ms("46x", &parsed_ms) &&
          !frontend_config_parse_audio_buffer_ms("-1", &parsed_ms) &&
          !frontend_config_parse_audio_buffer_ms("2001", &parsed_ms), "CLI accepted invalid audio buffer");
    CHECK(audio_policy_period_frames(44100, 46) == 512 && audio_policy_period_frames(48000, 46) == 512 &&
          audio_policy_period_frames(48000, 0) == AUDIO_POLICY_SAFE_PERIOD_FRAMES,
          "audio period policy does not honor short/safe settings");
    CHECK(audio_policy_open_fallback(512, false) == AUDIO_POLICY_SAFE_PERIOD_FRAMES &&
          audio_policy_restore_safe(512, AUDIO_POLICY_UNDERRUN_LIMIT), "audio policy fallback");
    static const char stretch_bad[] = "{\"frontend\":{\"audio_stretch\":\"yes\"}}";
    CHECK(!frontend_config_parse(stretch_bad, sizeof(stretch_bad) - 1, &c, why, sizeof(why)), "audio_stretch \"yes\" accepted");

    static const char none[] = "{ \"id\": \"x\" }";
    CHECK(frontend_config_parse(none, sizeof(none) - 1, &c, why, sizeof(why)) && c.option_count == 0, "no frontend object");
    static const char * const bad[] = {
        "", "[]", "{", "{\"frontend\": {\"buttons\": []}}", "{\"frontend\": {\"buttons\": [\"turbo\"]}}",
        "{\"frontend\": {\"buttons\": [\"a\",]}}", "{\"frontend\": {\"options\": {\"k\": []}}}", "{\"frontend\": 1}",
        "{\"frontend\": {\"frameskip\": \"fast\"}}", "{\"frontend\": {\"skip_options\": {\"k\": {}}}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        c.option_count = 7;
        CHECK(!frontend_config_parse(bad[i], strlen(bad[i]), &c, why, sizeof(why)), "accepted bad manifest %zu", i);
        CHECK(c.option_count == 0, "bad manifest %zu left options behind", i);
    }
}

static void check_layouts(void) {
    /* Portrait WonderSwan keeps its 144:224 aspect while giving controls 360 rows. */
    video_scale_t portrait;
    frontend_config_t config;
    char why[200];
    static const char ws_layout[] = "{\"frontend\":{\"game_height_percent\":55}}";
    CHECK(frontend_config_parse(ws_layout, sizeof(ws_layout) - 1, &config, why, sizeof(why)), "%s", why);
    int cap = 800 * (int) config.game_height_percent / 100;
    CHECK(cap == 440 && video_scale_init(&portrait, 144, 224, 480, cap) &&
          portrait.game_h == 440 && portrait.draw_w == 282 && portrait.draw_x0 == 99,
          "portrait WonderSwan viewport lost its aspect or height cap");
    uint32_t ws_buttons = BTN(RETRO_DEVICE_ID_JOYPAD_B) | BTN(RETRO_DEVICE_ID_JOYPAD_A) |
                          BTN(RETRO_DEVICE_ID_JOYPAD_X) | BTN(RETRO_DEVICE_ID_JOYPAD_Y) |
                          BTN(RETRO_DEVICE_ID_JOYPAD_L) | BTN(RETRO_DEVICE_ID_JOYPAD_R) |
                          BTN(RETRO_DEVICE_ID_JOYPAD_SELECT) | BTN(RETRO_DEVICE_ID_JOYPAD_START);
    CHECK(frontend_layout_check(480, 800, cap, ws_buttons, why, sizeof(why)), "%s", why);
    static const struct { int w, h; } screens[] = { { 480, 800 }, { 480, 720 }, { 320, 480 } };
    static const uint32_t sets[] = {
        BTN(RETRO_DEVICE_ID_JOYPAD_B) | BTN(RETRO_DEVICE_ID_JOYPAD_A) | BTN(RETRO_DEVICE_ID_JOYPAD_SELECT) | BTN(RETRO_DEVICE_ID_JOYPAD_START),
        BTN(RETRO_DEVICE_ID_JOYPAD_B) | BTN(RETRO_DEVICE_ID_JOYPAD_A) | BTN(RETRO_DEVICE_ID_JOYPAD_L) |
            BTN(RETRO_DEVICE_ID_JOYPAD_R) | BTN(RETRO_DEVICE_ID_JOYPAD_SELECT) | BTN(RETRO_DEVICE_ID_JOYPAD_START),
        BTN(RETRO_DEVICE_ID_JOYPAD_B) | BTN(RETRO_DEVICE_ID_JOYPAD_A) | BTN(RETRO_DEVICE_ID_JOYPAD_X) |
            BTN(RETRO_DEVICE_ID_JOYPAD_Y) | BTN(RETRO_DEVICE_ID_JOYPAD_L) | BTN(RETRO_DEVICE_ID_JOYPAD_R) |
            BTN(RETRO_DEVICE_ID_JOYPAD_SELECT) | BTN(RETRO_DEVICE_ID_JOYPAD_START),
        BTN(RETRO_DEVICE_ID_JOYPAD_A) | BTN(RETRO_DEVICE_ID_JOYPAD_START),
        BTN(RETRO_DEVICE_ID_JOYPAD_B) | BTN(RETRO_DEVICE_ID_JOYPAD_X) | BTN(RETRO_DEVICE_ID_JOYPAD_SELECT),
    };
    /* Picture shapes: 240x160, 240x224, 160x144, 256x192, 320x240 scaled to the width, then capped at 68%. */
    static const int shapes[][2] = { { 240, 160 }, { 240, 224 }, { 160, 144 }, { 256, 192 }, { 320, 240 }, { 256, 239 } };
    for (size_t sc = 0; sc < sizeof(screens) / sizeof(screens[0]); sc++)
        for (size_t sh = 0; sh < sizeof(shapes) / sizeof(shapes[0]); sh++) {
            int game = shapes[sh][1] * screens[sc].w / shapes[sh][0];
            if (game > screens[sc].h * 68 / 100) game = screens[sc].h * 68 / 100;
            for (size_t b = 0; b < sizeof(sets) / sizeof(sets[0]); b++) {
                char why[200];
                CHECK(frontend_layout_check(screens[sc].w, screens[sc].h, game, sets[b], why, sizeof(why)), "%s", why);
            }
        }
}


/* ---- audio stretch controller against a simulated device ---- */

typedef struct {
    double t, fill;        /* seconds, frames in the device buffer */
    bool running;          /* the device starts once the buffer has been filled */
    unsigned underruns, underruns_late;
    double late_after;     /* underruns after this time count as "late" */
} sim_dev_t;

#define SIM_BUFFER 4096u
#define SIM_PERIOD 1024u
#define SIM_RATE 48000u

static void sim_advance(sim_dev_t * d, double dt) {
    d->t += dt;
    if (!d->running) return;
    d->fill -= SIM_RATE * dt;
    if (d->fill <= 0) {
        d->fill = 0;
        d->running = false;
        d->underruns++;
        if (d->t > d->late_after) d->underruns_late++;
    }
}

/* Blocking write like ALSA's: wait for a period of room, copy what fits, repeat. */
static void sim_write(sim_dev_t * d, unsigned frames) {
    while (frames) {
        double room = SIM_BUFFER - d->fill;
        if (room < SIM_PERIOD && room < frames) {
            if (!d->running) room = 0; /* never happens: a stopped device fills up and starts */
            sim_advance(d, (SIM_PERIOD - room) / SIM_RATE);
            room = SIM_BUFFER - d->fill;
        }
        unsigned c = (double) frames < room ? frames : (unsigned) room;
        d->fill += c;
        frames -= c;
        if (!d->running && d->fill >= SIM_BUFFER) d->running = true;
    }
}

typedef struct {
    double seconds_speed[2]; /* producer speed (1.0 = real time) before and after switch_at */
    double switch_at, duration, jitter;
    double window_max_change;   /* largest |ratio(t+1s) - ratio(t)| seen */
    double ratio_end_avg;       /* average ratio over the last 10 s */
    uint32_t q_min, q_max;
    unsigned underruns, underruns_late;
    double final_ratio;
    stretch_stats_t stats;
} sim_result_t;

static void run_sim(double speed_a, double speed_b, double switch_at, double duration, double jitter, sim_result_t * r) {
    enum { IN_FRAMES = 800, MAX_FRAMES = 20000 };
    static double times[MAX_FRAMES];
    static double ratios[MAX_FRAMES];
    memset(r, 0, sizeof(*r));
    sim_dev_t d = { 0 };
    d.late_after = 15.0;
    stretch_t st;
    stretch_init(&st, SIM_BUFFER, SIM_RATE);
    uint32_t q = STRETCH_Q16_ONE;
    r->q_min = r->q_max = q;
    unsigned seed = 12345, n = 0;
    double end_sum = 0, end_w = 0;
    while (d.t < duration && n < MAX_FRAMES) {
        const double speed = d.t < switch_at ? speed_a : speed_b;
        seed = seed * 1103515245u + 12345u;
        const double noise = jitter * ((double) ((seed >> 16) & 0x7FFF) / 16383.5 - 1.0);
        sim_advance(&d, (1.0 / 60.0) / (speed * (1.0 + noise)));
        const unsigned out = (unsigned) (((uint64_t) IN_FRAMES * STRETCH_Q16_ONE + q / 2) / q);
        sim_write(&d, out);
        q = stretch_update(&st, (unsigned) d.fill, out);
        if (q < r->q_min) r->q_min = q;
        if (q > r->q_max) r->q_max = q;
        times[n] = d.t;
        ratios[n] = (double) q / STRETCH_Q16_ONE;
        if (d.t > duration - 10.0) {
            end_sum += ratios[n] * out;
            end_w += out;
        }
        n++;
    }
    /* Largest ratio change over any one-second window. */
    for (unsigned i = 0, j = 0; i < n; i++) {
        while (j + 1 < n && times[j] < times[i] + 1.0) j++;
        if (times[j] >= times[i] + 1.0) {
            const double ch = fabs(ratios[j] - ratios[i]);
            if (ch > r->window_max_change) r->window_max_change = ch;
        }
    }
    r->ratio_end_avg = end_w > 0 ? end_sum / end_w : 1.0;
    r->underruns = d.underruns;
    r->underruns_late = d.underruns_late;
    r->final_ratio = ratios[n - 1];
    stretch_stats(&st, &r->stats);
}

static void check_stretch(void) {
    sim_result_t r;
    /* A producer at or above real time: the ratio never leaves 1.0 and the device never runs dry. */
    run_sim(1.0, 1.0, 0, 60, 0, &r);
    CHECK(r.q_min == STRETCH_Q16_ONE && r.q_max == STRETCH_Q16_ONE, "100%%: ratio left 1.0 (%u..%u)", r.q_min, r.q_max);
    CHECK(r.underruns == 0, "100%%: %u underruns", r.underruns);
    run_sim(1.05, 1.05, 0, 60, 0.08, &r);
    CHECK(r.q_min == STRETCH_Q16_ONE && r.q_max == STRETCH_Q16_ONE, "105%% with jitter: ratio left 1.0 (%u..%u)", r.q_min, r.q_max);
    CHECK(r.underruns == 0, "105%% with jitter: %u underruns", r.underruns);
    printf("stretch 100%%: ratio 1.000 throughout, underruns %u\n", r.underruns);

    static const double speeds[] = { 0.95, 0.90, 0.80 };
    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        const double sp = speeds[i];
        run_sim(sp, sp, 0, 90, 0.03, &r);
        printf("stretch %.0f%%: underruns %u (after 15 s: %u), end ratio %.4f, min %.3f, max change per second %.3f, "
               "stretched %.0f%%, fill %.0f%%\n", sp * 100, r.underruns, r.underruns_late, r.ratio_end_avg,
               r.stats.min_ratio, r.window_max_change, r.stats.stretched_percent, r.stats.avg_fill_percent);
        CHECK(r.underruns_late == 0, "%.0f%%: %u underruns after convergence", sp * 100, r.underruns_late);
        CHECK(fabs(r.ratio_end_avg - sp) < 0.02, "%.0f%%: ratio %.4f did not converge to the producer speed", sp * 100,
              r.ratio_end_avg);
        CHECK(r.window_max_change <= STRETCH_MAX_DOWN + 0.005, "%.0f%%: ratio changed %.3f in one second", sp * 100,
              r.window_max_change);
        CHECK(r.stats.min_ratio >= STRETCH_MIN_RATIO - 1e-9, "%.0f%%: ratio %.3f under the minimum", sp * 100,
              r.stats.min_ratio);
    }
    /* Falls behind after 20 s, catches up again at 60 s: the ratio comes back to exactly 1.0. */
    run_sim(1.0, 0.85, 20, 140, 0.02, &r);
    CHECK(r.stats.min_ratio < 0.9, "slowdown not followed (min ratio %.3f)", r.stats.min_ratio);
    sim_result_t back;
    run_sim(0.85, 1.0, 40, 140, 0.02, &back);
    printf("stretch recovery: back at 1.0 = %s (final ratio %.4f), underruns after 15 s %u\n",
           back.final_ratio == 1.0 ? "yes" : "no", back.final_ratio, back.underruns_late);
    CHECK(back.final_ratio == 1.0, "ratio did not return to 1.0 (%.4f)", back.final_ratio);
    CHECK(back.window_max_change <= STRETCH_MAX_DOWN + 0.005, "recovery changed %.3f in one second", back.window_max_change);

    /* The resampler step: exact at 1.0, base / ratio otherwise. */
    const uint64_t base = (uint64_t) (223721.0 / 48000.0 * 4294967296.0);
    CHECK(stretch_step(base, STRETCH_Q16_ONE) == base, "step at ratio 1.0 changed");
    CHECK(stretch_step(base, STRETCH_Q16_ONE / 2) == base * 2, "step at ratio 0.5 is not twice the base");
    const double want = (double) base / 0.8;
    CHECK(fabs((double) stretch_step(base, (uint32_t) (0.8 * STRETCH_Q16_ONE + 0.5)) - want) < want * 1e-4,
          "step at ratio 0.8");
}


/* ---- stretch fill reading ---- */

static void check_fill_reading(void) {
    CHECK(stretch_fill_from_avail(0, 4096) == 4096, "avail 0 is a full buffer");
    CHECK(stretch_fill_from_avail(4096, 4096) == 0, "avail = buffer is an empty buffer");
    CHECK(stretch_fill_from_avail(1000, 4096) == 3096, "avail 1000 of 4096");
    CHECK(stretch_fill_from_avail(4500, 4096) == 0, "avail above the buffer (underrun) is fill 0");
    CHECK(stretch_fill_from_avail(8192, 4096) == 0, "avail at twice the buffer is still fill 0");
    CHECK(stretch_fill_from_avail(8193, 4096) == -1, "avail beyond twice the buffer is not a reading");
    CHECK(stretch_fill_from_avail(0xFFFFFF00u, 4096) == -1, "wrapped avail is not a reading");
    CHECK(stretch_fill_from_avail(10, 0) == -1, "no buffer, no reading");
}

/* ---- underrun accounting with a simulated device ---- */

typedef struct {
    int script[16];                  /* per call: frames to accept (>= 0), or -errno */
    int calls, prepares, prepare_ok;
    unsigned accepted;               /* frames that went out */
    int16_t first_sample[64];
    int received;
} sim_pcm_t;

static int xsim_write(void * ctx, const int16_t * data, unsigned frames) {
    sim_pcm_t * d = ctx;
    int step = d->calls < 16 ? d->script[d->calls] : -1000;
    d->calls++;
    if (step < 0) {
        errno = -step;
        return -1;
    }
    unsigned n = (unsigned) step > frames ? frames : (unsigned) step;
    if (step == 0) n = frames; /* 0 in the script: accept everything */
    if (d->received < 64) d->first_sample[d->received++] = data[0];
    d->accepted += n;
    return (int) n;
}

static bool xsim_prepare(void * ctx) {
    sim_pcm_t * d = ctx;
    d->prepares++;
    return d->prepare_ok;
}

static void check_xrun(void) {
    int16_t audio[200 * 2];
    for (int i = 0; i < 200 * 2; i++) audio[i] = (int16_t) i;

    sim_pcm_t d = { .prepare_ok = 1 };
    xrun_stats_t st = { 0 };
    xrun_device_t dev = { xsim_write, xsim_prepare, &d };
    CHECK(xrun_write_all(&dev, &st, audio, 200) && d.accepted == 200 && !st.underruns && !d.prepares, "clean write");

    memset(&d, 0, sizeof(d));
    memset(&st, 0, sizeof(st));
    d.prepare_ok = 1;
    d.script[0] = -EPIPE; /* underrun: recover and write again */
    CHECK(xrun_write_all(&dev, &st, audio, 200) && d.accepted == 200 && st.underruns == 1 && st.prepares == 1 &&
          d.prepares == 1 && st.errors == 0, "EPIPE not counted or not recovered (underruns %u)", st.underruns);

    /* A write the kernel stopped half way through: 80 frames out, then the xrun, then the other 120.
     * The second call's first sample must continue where the first stopped. */
    memset(&d, 0, sizeof(d));
    memset(&st, 0, sizeof(st));
    d.prepare_ok = 1;
    d.script[0] = 80;
    d.script[1] = -EPIPE;
    CHECK(xrun_write_all(&dev, &st, audio, 200) && d.accepted == 200 && st.underruns == 1 &&
          d.received == 2 && d.first_sample[0] == 0 && d.first_sample[1] == 160, "partial write then xrun lost audio");

    memset(&d, 0, sizeof(d));
    memset(&st, 0, sizeof(st));
    d.prepare_ok = 1;
    d.script[0] = -ESTRPIPE;
    CHECK(xrun_write_all(&dev, &st, audio, 200) && st.suspends == 1 && st.underruns == 0 && st.prepares == 1,
          "ESTRPIPE accounting");

    memset(&d, 0, sizeof(d));
    memset(&st, 0, sizeof(st));
    d.prepare_ok = 1;
    for (int i = 0; i < 16; i++) d.script[i] = -EPIPE;
    CHECK(!xrun_write_all(&dev, &st, audio, 200) && st.underruns == XRUN_MAX_RECOVERIES && st.errors == 1 &&
          d.accepted == 0, "endless EPIPE must give up after %d recoveries (got %u underruns)", XRUN_MAX_RECOVERIES,
          st.underruns);

    memset(&d, 0, sizeof(d));
    memset(&st, 0, sizeof(st));
    d.script[0] = -EPIPE;
    CHECK(!xrun_write_all(&dev, &st, audio, 200) && st.underruns == 1 && st.errors == 1, "failed prepare");

    memset(&d, 0, sizeof(d));
    memset(&st, 0, sizeof(st));
    d.prepare_ok = 1;
    d.script[0] = -EIO;
    CHECK(!xrun_write_all(&dev, &st, audio, 200) && st.underruns == 0 && st.errors == 1 && d.prepares == 0,
          "another error is not an underrun");
}

/* ---- presenter hand-off ---- */

typedef struct {
    pthread_mutex_t lock;
    int delay_us;
    int shown_count, last_value, last_rows;
    int values[512];
    int in_show;                     /* show callbacks running at once: must never exceed 1 */
    int max_in_show;
} sim_screen_t;

static void sim_show(void * ctx, const uint16_t * px, int rows) {
    sim_screen_t * sc = ctx;
    pthread_mutex_lock(&sc->lock);
    if (++sc->in_show > sc->max_in_show) sc->max_in_show = sc->in_show;
    pthread_mutex_unlock(&sc->lock);
    if (sc->delay_us) usleep((useconds_t) sc->delay_us);
    pthread_mutex_lock(&sc->lock);
    if (sc->shown_count < 512) sc->values[sc->shown_count] = px[0];
    sc->shown_count++;
    sc->last_value = px[rows * 4 - 1]; /* last pixel of the last row: the whole frame was written */
    sc->last_rows = rows;
    sc->in_show--;
    pthread_mutex_unlock(&sc->lock);
}

static void draw_sim_frame(present_t * p, int value, int rows) {
    uint16_t * buf = present_buffer(p);
    for (int i = 0; i < rows * 4; i++) buf[i] = (uint16_t) value;
}

static void check_present(void) {
    sim_screen_t sc = { .lock = PTHREAD_MUTEX_INITIALIZER, .delay_us = 3000 };
    present_t p;
    CHECK(present_start(&p, 64 * 4 * 2, sim_show, &sc), "presenter did not start");

    /* A producer much faster than the screen: submit never waits for it and latest wins. */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int v = 1; v <= 200; v++) {
        draw_sim_frame(&p, v, 8 + v % 8);
        present_submit(&p, 8 + v % 8);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double submit_ms = (double) (t1.tv_sec - t0.tv_sec) * 1e3 + (double) (t1.tv_nsec - t0.tv_nsec) / 1e6;
    CHECK(submit_ms < 200 * 3.0 * 0.5, "200 submits took %.1f ms: the producer waited for the screen", submit_ms);

    /* The final frame is shown before stop returns, and the shown ones are in order. */
    present_stop(&p);
    CHECK(sc.last_value == 200 && sc.last_rows == 8 + 200 % 8, "the final frame was lost (last shown %d, %d rows)",
          sc.last_value, sc.last_rows);
    CHECK(p.submitted == 200 && p.shown + p.dropped == p.submitted && p.shown < 200,
          "submitted %llu shown %llu dropped %llu", (unsigned long long) p.submitted, (unsigned long long) p.shown,
          (unsigned long long) p.dropped);
    for (int i = 1; i < sc.shown_count && i < 512; i++)
        CHECK(sc.values[i] > sc.values[i - 1], "frames shown out of order: %d after %d", sc.values[i], sc.values[i - 1]);
    CHECK(sc.max_in_show == 1, "show ran %d times at once", sc.max_in_show);

    /* A fast screen shows every frame that is given time to be shown. */
    sc = (sim_screen_t) { .lock = PTHREAD_MUTEX_INITIALIZER };
    CHECK(present_start(&p, 64 * 4 * 2, sim_show, &sc), "presenter did not start (2)");
    for (int v = 1; v <= 20; v++) {
        draw_sim_frame(&p, v, 10);
        present_submit(&p, 10);
        usleep(2000);
    }
    present_stop(&p);
    CHECK(sc.shown_count == 20 && sc.last_value == 20, "fast screen showed %d of 20 frames", sc.shown_count);

    /* A claim (controls, pause menu) discards the waiting frame, waits for a show in progress and keeps the
     * presenter out until released, so nothing writes the screen behind its back. */
    sc = (sim_screen_t) { .lock = PTHREAD_MUTEX_INITIALIZER, .delay_us = 20000 };
    CHECK(present_start(&p, 64 * 4 * 2, sim_show, &sc), "presenter did not start (3)");
    draw_sim_frame(&p, 1, 10);
    present_submit(&p, 10);
    usleep(5000);                    /* frame 1 is being shown */
    draw_sim_frame(&p, 2, 10);
    present_submit(&p, 10);          /* frame 2 waits */
    present_claim(&p);
    CHECK(sc.in_show == 0 && sc.shown_count == 1, "claim returned during a show (%d in show, %d shown)", sc.in_show,
          sc.shown_count);
    usleep(60000);
    CHECK(sc.shown_count == 1, "the presenter wrote while the screen was claimed");
    present_release(&p);
    usleep(60000);
    CHECK(sc.shown_count == 1, "the frame dropped by the claim was shown after it");
    draw_sim_frame(&p, 3, 10);
    present_submit(&p, 10);
    present_stop(&p);
    CHECK(sc.shown_count == 2 && sc.last_value == 3, "frames after the claim: %d shown, last %d", sc.shown_count,
          sc.last_value);
}

/* "selftest --write-rom PATH" only writes the generated ROM, so the device
 * binary can be exercised with --bench under qemu. */
int main(int argc, char ** argv) {
    if (argc == 3 && strcmp(argv[1], "--write-rom") == 0) return write_test_rom(argv[2]) ? 0 : 1;
    if (argc == 8 && strcmp(argv[1], "--preview") == 0) /* W H GAME_H BUTTON_MASK MENU(0/1) PPM */
        return frontend_preview(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), (uint32_t) strtoul(argv[5], NULL, 0),
                                atoi(argv[6]) != 0, argv[7]) ? 0 : 1;
    char * bad_stats_zero[] = { (char *) "selftest", (char *) "unused.rom", (char *) "--stats-interval", (char *) "0" };
    char * bad_stats_text[] = { (char *) "selftest", (char *) "unused.rom", (char *) "--stats-interval", (char *) "1x" };
    CHECK(frontend_main(4, bad_stats_zero) == 2, "--stats-interval 0 was accepted");
    CHECK(frontend_main(4, bad_stats_text) == 2, "non-numeric --stats-interval was accepted");
    char path[] = "/tmp/frontend_selftest_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);
    if (!write_test_rom(path)) {
        fprintf(stderr, "cannot write %s\n", path);
        unlink(path);
        return 1;
    }

    CHECK(frontend_video_pipeline_check(), "native video pipeline did not preserve its last frame");
    frontend_bench_t r;
    bool loaded = frontend_bench(path, FRAMES, &r);
    unlink(path);
    CHECK(loaded, "the core did not load the test ROM");
    check_config();
    check_stretch();
    check_fill_reading();
    check_xrun();
    check_present();
    check_layouts();
    if (loaded) {
        printf("video %ux%u scaled to %dx%d, %llu video frames, audio %llu frames, %.0f Hz at %.3f fps, sram %zu bytes\n",
               r.width, r.height, r.scaled_width, r.scaled_height, (unsigned long long) r.video_frames,
               (unsigned long long) r.audio_frames, r.sample_rate, r.fps, r.sram_size);
        CHECK(r.width == 240 && r.height == 224, "video geometry %ux%u, expected 240x224", r.width, r.height);
        CHECK(r.video_frames == (uint64_t) FRAMES, "%llu video frames for %d emulated frames",
              (unsigned long long) r.video_frames, FRAMES);
        CHECK(r.width && r.scaled_height == (int) ((uint64_t) r.height * (uint64_t) r.scaled_width / r.width),
              "scaled height %d is not width %d * %u / %u", r.scaled_height, r.scaled_width, r.height, r.width);
        double seconds = FRAMES / r.fps;
        double per_second = r.audio_frames / seconds;
        CHECK(r.sample_rate > 0 && per_second > r.sample_rate * 0.99 && per_second < r.sample_rate * 1.01,
              "%.1f audio frames per emulated second, core rate %.0f", per_second, r.sample_rate);
        CHECK(r.sram_size == 0x2000, "battery RAM size %zu, expected 8192", r.sram_size);
        printf("save state %zu bytes, round trip %d\n", r.state_size, r.state_roundtrip);
        CHECK(r.state_roundtrip == 1, "save state round trip did not reproduce the state (%d)", r.state_roundtrip);
    }
    if (failures) return 1;
    printf("frontend selftest: PASS\n");
    return 0;
}
