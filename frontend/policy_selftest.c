#include "audio_policy.h"
#include "residency.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_audio(void) {
    const uint32_t rates[] = {32000, 44100, 48000, 96000};
    const uint32_t durations[] = {1, 20, 46, 90, 250};
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
        for (unsigned d = 0; d < sizeof(durations) / sizeof(durations[0]); ++d) {
            uint32_t p = audio_policy_period_frames(rates[r], durations[d]);
            assert(p >= AUDIO_POLICY_MIN_PERIOD_FRAMES && p <= AUDIO_POLICY_MAX_PERIOD_FRAMES);
            assert((p & (p - 1)) == 0);
        }
    }
    assert(audio_policy_period_frames(44100, 46) == 512);
    assert(audio_policy_period_frames(48000, 46) == 512);
    assert(audio_policy_period_frames(32000, 46) == 256);
    assert(audio_policy_period_frames(96000, 46) == 1024);
    assert(audio_policy_period_frames(48000, 0) == AUDIO_POLICY_SAFE_PERIOD_FRAMES);
    assert(audio_policy_period_frames(0, 46) == AUDIO_POLICY_SAFE_PERIOD_FRAMES);
    assert(audio_policy_period_frames(UINT32_MAX, UINT32_MAX) == AUDIO_POLICY_SAFE_PERIOD_FRAMES);
    assert(audio_policy_open_fallback(512, false) == 1024);
    assert(audio_policy_open_fallback(512, true) == 512);
    assert(audio_policy_open_fallback(1024, false) == 1024);
    assert(!audio_policy_restore_safe(512, 2));
    assert(audio_policy_restore_safe(512, 3));
    assert(!audio_policy_restore_safe(1024, 99));
}

static void check_residency(void) {
    residency_map_t m; uintptr_t start = 0, end = 0;
    assert(residency_parse_map("00400000-00402000 r-xp 00000000 08:01 99 /opt/game", &m));
    assert(m.start == 0x400000 && m.end == 0x402000 && m.executable && !m.writable);
    assert(residency_select_code_range(&m, "/opt/game", 0x400100, 0x401101,
                                       4096, &start, &end));
    assert(start == 0x400000 && end == 0x402000);
    assert(!residency_select_code_range(&m, "/other", 0x400100, 0x401000,
                                        4096, &start, &end));
    assert(residency_select_code_range(&m, "/opt/game", 0x400100, 0x401000,
                                       4096, &start, &end));
    assert(start == 0x400000 && end == 0x401000);
    assert(residency_parse_map("00400000-00402000 rwxp 00000000 08:01 99 /opt/game", &m));
    assert(!residency_select_code_range(&m, "/opt/game", 0x400100, 0x401000,
                                        4096, &start, &end));
    assert(residency_parse_map("00400000-00402000 r-xp 00000000 08:01 99 [vdso]", &m));
    assert(!residency_select_code_range(&m, "/opt/game", 0x400100, 0x401000,
                                        4096, &start, &end));
    assert(!residency_parse_map("bad line", &m));
}

int main(void) {
    check_audio(); check_residency();
    puts("audio and residency policy selftest: ok");
    return 0;
}
