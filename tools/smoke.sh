#!/usr/bin/env bash
# Runs the device executable of each given emulator id under qemu-mipsel with
# --bench on a test ROM from tests/roms/ (see tools/fetch_test_roms.sh), from
# the emulator's own folder like the player does. It is a functional check
# (the core loads, runs, produces video and audio, and its save state round
# trip works); qemu's speed says nothing about the device. Skipped cleanly when
# qemu-mipsel or a ROM is missing.
set -uo pipefail

roms="${ROMS:-tests/roms}"
frames="${FRAMES:-600}"
if ! command -v qemu-mipsel >/dev/null 2>&1; then
    echo "smoke: qemu-mipsel not found, skipping"
    exit 0
fi

rom_for() {
    case "$1" in
        quicknes) echo test.nes ;;
        gpsp) echo 240pee_mb.gba ;;
        gearboy) echo gb240p.gb ;;
        gearsystem) echo "test.sms test.gg test.sg" ;;
        race) echo "test.ngp" ;;
        wswan) echo "test.ws test.wsc" ;;
        pce) echo "test.pce" ;;
        prboom) echo "freedoom1.wad" ;;
        *) echo "" ;;
    esac
}

# The folder an emulator runs from (the player uses its install folder): its
# own folder, plus the engine data file an emulator needs next to it.
run_dir() {
    case "$1" in
        prboom)
            mkdir -p build/smoke/prboom
            cp emulators/prboom/emulator.json third_party/prboom/prboom.wad build/smoke/prboom/
            echo build/smoke/prboom ;;
        *) echo "emulators/$1" ;;
    esac
}

root=$(pwd)
failed=0
for id in "$@"; do
    exe="$root/build/target/$id/$id"
    list=$(rom_for "$id")
    [ -x "$exe" ] || { echo "smoke: $id: no executable, build first"; failed=1; continue; }
    [ -n "$list" ] || { echo "smoke: $id: no test ROM known, skipped"; continue; }
    for rom in $list; do
        dir="$roms"
        [ "$rom" = freedoom1.wad ] && dir=third_party/freedoom
        if [ ! -f "$dir/$rom" ]; then
            echo "smoke: $id: $dir/$rom missing (tools/fetch_test_roms.sh), skipped"
            continue
        fi
        out=$(cd "$(run_dir "$id")" && timeout 300 qemu-mipsel "$exe" "$root/$dir/$rom" --bench "$frames" 2>&1)
        status=$?
        speed=$(printf '%s\n' "$out" | sed -n 's/.*speed \([0-9.]*\) fps.*/\1/p')
        video=$(printf '%s\n' "$out" | sed -n 's/^video \([0-9x]*\) .*/\1/p')
        state=$(printf '%s\n' "$out" | sed -n 's/.*state round trip \(.*\)/\1/p')
        if [ $status -ne 0 ] || [ -z "$speed" ]; then
            echo "smoke: $id $rom: FAILED (exit $status)"
            printf '%s\n' "$out" | tail -5
            failed=1
        else
            echo "smoke: $id $rom: ok, video $video, qemu $speed fps, state round trip $state"
        fi
    done
done
exit $failed
