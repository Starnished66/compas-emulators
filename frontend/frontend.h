#ifndef FRONTEND_H
#define FRONTEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Entry point of an emulator program: argv[1] is the ROM path and an
 * optional "--bench N" runs N frames headless instead of taking over the
 * screen. "--opt key=value" (repeatable) answers a core option and
 * "--frameskip off|auto|half" sets the frame skipping and "--skip-opt key=value" the
 * options used while video is skipped, all over emulator.json;
 * "--audio-stretch on|off" overrides "audio_stretch" (frontend/stretch.h);
 * "--stats-interval N" logs interactive performance and process memory every N seconds;
 * "--budget-ms N" gives the frameskip policy of a bench another frame budget.
 * Returns the process exit status: 0 for a normal quit, 1 when the
 * ROM or display could not be opened, 2 for bad arguments, and the process
 * itself exits with 3 if the main loop stalls. The frontend is a minimal
 * libretro frontend; the core is linked statically. */
int frontend_main(int argc, char ** argv);

typedef struct {
    unsigned width, height;          /* geometry of the last video frame */
    int scaled_width, scaled_height; /* that frame scaled into the 480x800 test surface */
    double sample_rate, fps;         /* as reported by the core */
    uint64_t frames, video_frames, audio_frames;
    uint64_t video_skipped;          /* frames whose video was dropped by the frameskip setting */
    double wall_s, cpu_s;
    double audio_s, scale_s;         /* time spent in the audio path (resampling) and in scaling video */
    uint32_t audio_hash, video_hash; /* of a few untimed frames after the run: equal builds give equal hashes */
    int audio_peak;                  /* largest sample magnitude of those frames: 0 means silence */
    long vm_hwm_kb;
    size_t sram_size;                /* battery RAM the core reports; no file is read or written */
    size_t state_size;               /* save state size the core reports (0: none) */
    int state_roundtrip;             /* serialize, run, unserialize, serialize: 1 identical, 0 not, -1 unsupported */
} frontend_bench_t;

/* Runs the ROM headless and unthrottled for the given number of frames,
 * scaling every frame into a RAM surface. False if the core refuses the
 * ROM. The libretro core is process-global: call this at most once. */
bool frontend_bench(const char * rom, long frames, frontend_bench_t * out);

/* Checks the touch layout for a screen size, game area height and button
 * set (bit n of buttons = RETRO_DEVICE_ID_JOYPAD_<n>): every control inside
 * the control area, no overlaps, each control pressed by its own middle. */
bool frontend_layout_check(int width, int height, int game_height, uint32_t buttons, char * error, size_t error_size);

/* Renders the controls (and the pause menu over them) for a screen size into a PPM file. */
bool frontend_preview(int width, int height, int game_height, uint32_t buttons, bool with_menu, const char * ppm_path);

#ifdef HOST_BUILD
/* Checks raw-frame lifetime, async scaling and dynamic geometry in RAM. */
bool frontend_video_pipeline_check(void);
#endif

#endif /* FRONTEND_H */
