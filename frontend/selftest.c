/* Host self-test for the frontend: builds a minimal iNES ROM, runs it
 * through the headless benchmark path and checks the frontend's video and
 * audio plumbing against the statically linked QuickNES core. */
#include "frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* "selftest --write-rom PATH" only writes the generated ROM, so the device
 * binary can be exercised with --bench under qemu. */
int main(int argc, char ** argv) {
    if (argc == 3 && strcmp(argv[1], "--write-rom") == 0) return write_test_rom(argv[2]) ? 0 : 1;
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

    frontend_bench_t r;
    bool loaded = frontend_bench(path, FRAMES, &r);
    unlink(path);
    CHECK(loaded, "the core did not load the test ROM");
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
    }
    if (failures) return 1;
    printf("frontend selftest: PASS\n");
    return 0;
}
