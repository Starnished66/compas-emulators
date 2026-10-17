#!/usr/bin/env bash
# Builds the ClownMDEmu speed-test variants into build/target/clownmd-variants/
# (static stripped mipsel executables, described in VARIANTS.txt there) so they
# can be timed on a device in one pass. Each variant is a make run with its
# own build folder, compiler flags and, for the ones that are only run options,
# its own command line arguments (they go in VARIANTS.txt, third column).
#
#   tools/clownmd_variants.sh [variant ...]    all variants when none is named
#   tools/clownmd_variants.sh --profile        regenerates the profile the pgo variants use
#                                              (experimental/clownmd/profile/, needs qemu-mipsel and the test ROMs)
#
# Timing: tools/clownmd_bench.sh (qemu), or on the device
#   NAME ROM --bench 900 ARGS...
set -euo pipefail

out=build/target/clownmd-variants
core_base="-std=gnu11 -O2 -DNDEBUG -DCLOWNCD_NO_CHD -DCC_RESAMPLER -DCC_RESAMPLER_NO_HIGHPASS -fomit-frame-pointer -w -Ithird_party/clownmd -Ithird_party/clownmd/libretro-common/include"
cpu_base="-march=mips32r2 -mabi=32 -mfp32 -mno-odd-spreg -ffp-contract=off"
nopic="-fno-pic -mno-abicalls -mno-shared"
profile=experimental/clownmd/profile/unity.gcda
pgo_use="-fprofile-use=$PWD/$profile -fprofile-correction -fprofile-partial-training -Wno-missing-profile"
skip_opts="--skip-opt clownmdemu_disable_sprite_plane=enabled --skip-opt clownmdemu_disable_window_plane=enabled --skip-opt clownmdemu_disable_plane_a=enabled --skip-opt clownmdemu_disable_plane_b=enabled"

# name | description | extra core flags (after the defaults) | extra cpu flags (core and frontend) | run arguments
align="-falign-functions=32 -falign-loops=16 -falign-jumps=8"
table() {
cat <<TABLE
baseline|The build measured on the R1 (42 fps): -O2, PIC code, the old frontend (no resampler in --bench)|@prebuilt||
fe-bench-base|The old frontend again but with the audio resampler included in --bench|@prebuilt||
fe-new|Current flags (-O2) with the new frontend: faster scaler, integer resampler|||
o3|-O3 instead of -O2|-O3||
unroll|-O2 -funroll-loops|-funroll-loops||
o3-unroll|-O3 -funroll-loops|-O3 -funroll-loops||
nopic|Non-PIC static code: no abicalls, no GOT calls ($nopic)||$nopic|
nopic-o3|nopic with -O3|-O3|$nopic|
nopic-os|nopic with -Os (smaller code, fewer instruction cache misses)|-Os|$nopic|
nopic-o1|nopic with -O1|-O1|$nopic|
nopic-unroll|nopic with -funroll-loops|-funroll-loops|$nopic|
nopic-nozerochk|nopic with -mno-check-zero-division||$nopic -mno-check-zero-division|
nopic-align|nopic with $align|$align|$nopic|
nopic-24kf|nopic tuned for 24kf (single issue, short pipe)||$nopic -mtune=24kf|
nopic-74kc|nopic tuned for 74kc (dual issue)||$nopic -mtune=74kc|
nopic-1004kc|nopic tuned for 1004kc||$nopic -mtune=1004kc|
nopic-lto|nopic with -flto||$nopic -flto|
pgo|nopic with profile guided optimization (profile from the two SGDK test ROMs, partial training)|$pgo_use|$nopic|
fast|nopic, no zero-division checks, aligned functions and loops (the flag set the options below build on)|$align|$nopic -mno-check-zero-division|
fast-os|fast with -Os|$align -Os|$nopic -mno-check-zero-division|
lowpass-off|fast and the core's low-pass filters off (changes the sound slightly)|@same=fast||--opt clownmdemu_lowpass_filter=disabled
ladder-off|fast and the YM2612 ladder effect off (changes the sound slightly)|@same=fast||--opt clownmdemu_ladder_effect=disabled
audio-lite|fast with low-pass filters and ladder effect off|@same=fast||--opt clownmdemu_lowpass_filter=disabled --opt clownmdemu_ladder_effect=disabled
skip-half|fast, frameskip half: every second frame's video dropped by the frontend (the core ignores the call)|@same=fast||--frameskip half
skip-auto|fast, frameskip auto: drop the next frame's video after a frame over budget|@same=fast||--frameskip auto
skiplite-half|fast, frameskip half, and the VDP layers turned off on the skipped frames (skip options)|@same=fast||--frameskip half $skip_opts
skiplite-auto|fast, frameskip auto, and the VDP layers turned off on the skipped frames (skip options)|@same=fast||--frameskip auto $skip_opts
all-lite-auto|fast, audio-lite and skiplite-auto together (the most speed the options give)|@same=fast||--frameskip auto $skip_opts --opt clownmdemu_lowpass_filter=disabled --opt clownmdemu_ladder_effect=disabled
TABLE
}

build_one() {
    local name=$1 core_extra=$2 cpu_extra=$3 b=build/vbuild/$1
    if [[ "$core_extra" == @same=* ]]; then return 0; fi   # copied after the builds
    if [ "$core_extra" = "@prebuilt" ]; then
        [ -x "$out/$name" ] || { echo "$name: no prebuilt $out/$name (built once from the frontend before the speed work)" >&2; return 1; }
        return 0
    fi
    make -s BUILD="$b" clownmd_CFLAGS="$core_base $core_extra" TARGET_CPU_FLAGS="$cpu_base $cpu_extra" \
        "$b/target/clownmd/clownmd" >"$b.log" 2>&1 || { echo "build of $name failed, see $b.log" >&2; return 1; }
    cp "$b/target/clownmd/clownmd" "$out/$name"
}

if [ "${1:-}" = "--profile" ]; then
    b=build/vbuild/pgo-gen
    rm -rf "$b"
    mkdir -p "$b"
    # The same flags as the pgo variant (a profile only matches code compiled the same way), plus instrumentation.
    make -s BUILD="$b" clownmd_CFLAGS="$core_base -fprofile-generate -fprofile-update=single" \
        TARGET_CPU_FLAGS="$cpu_base $nopic -fprofile-generate" "$b/target/clownmd/clownmd" >"$b.log" 2>&1 ||
        { echo "instrumented build failed, see $b.log" >&2; exit 1; }
    for rom in sgdk_platformer sgdk_benchmark; do
        ${RUNNER-qemu-mipsel} "$b/target/clownmd/clownmd" "${ROMS:-tests/roms}/$rom.md" --bench "${FRAMES:-1500}" >/dev/null 2>&1
    done
    gcda=$(find "$b" -name 'unity.c.gcda' | head -n 1)
    [ -n "$gcda" ] || { echo "no profile was written" >&2; exit 1; }
    mkdir -p "$(dirname "$profile")"
    cp "$gcda" "$profile"
    echo "profile written to $profile"
    exit 0
fi

mkdir -p "$out" build/vbuild
want=" $* "
jobs_running=0
while IFS='|' read -r name desc core_extra cpu_extra args; do
    [ -z "$name" ] && continue
    if [ "$want" = "  " ] || [[ "$want" == *" $name "* ]]; then
        mkdir -p "build/vbuild/$name"
        build_one "$name" "$core_extra" "$cpu_extra" &
        jobs_running=$((jobs_running + 1))
        if [ "$jobs_running" -ge "${JOBS:-6}" ]; then wait -n; jobs_running=$((jobs_running - 1)); fi
    fi
done < <(table)
wait
# Variants that only differ in run arguments reuse the binary of another one.
while IFS='|' read -r name _ core_extra _; do
    if [[ "$core_extra" == @same=* ]] && { [ "$want" = "  " ] || [[ "$want" == *" $name "* ]]; }; then
        cp "$out/${core_extra#@same=}" "$out/$name"
    fi
done < <(table)
# One line per variant that exists: name | description | run arguments.
: >"$out/VARIANTS.txt"
while IFS='|' read -r name desc _ _ args; do
    [ -n "$name" ] && [ -x "$out/$name" ] && echo "$name | $desc | $args" >>"$out/VARIANTS.txt"
done < <(table)
echo "variants in $out"
