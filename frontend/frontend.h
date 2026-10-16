#ifndef FRONTEND_H
#define FRONTEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Entry point of an emulator program: argv[1] is the ROM path and an
 * optional "--bench N" runs N frames headless instead of taking over the
 * screen. Returns the process exit status: 0 for a normal quit, 1 when the
 * ROM or display could not be opened, 2 for bad arguments, and the process
 * itself exits with 3 if the main loop stalls. The frontend is a minimal
 * libretro frontend; the core is linked statically. */
int frontend_main(int argc, char ** argv);

typedef struct {
    unsigned width, height;          /* geometry of the last video frame */
    int scaled_width, scaled_height; /* that frame scaled into the 480x800 test surface */
    double sample_rate, fps;         /* as reported by the core */
    uint64_t frames, video_frames, audio_frames;
    double wall_s, cpu_s;
    long vm_hwm_kb;
    size_t sram_size;                /* battery RAM the core reports; no file is read or written */
} frontend_bench_t;

/* Runs the ROM headless and unthrottled for the given number of frames,
 * scaling every frame into a RAM surface. False if the core refuses the
 * ROM. The libretro core is process-global: call this at most once. */
bool frontend_bench(const char * rom, long frames, frontend_bench_t * out);

#endif /* FRONTEND_H */
