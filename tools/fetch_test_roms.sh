#!/usr/bin/env bash
# Downloads the freely licensed test ROMs the smoke test uses into tests/roms/
# (gitignored) and generates the original ones. Each download is checked
# against a pinned SHA-256.
#
# 240p Test Suite (mini), Damian Yerrick (pinobatch), v0.23, GPL-2.0-or-later:
# https://github.com/pinobatch/240p-test-mini
set -euo pipefail

dest="${1:-tests/roms}"
base="https://github.com/pinobatch/240p-test-mini/releases/download/v0.23"
mkdir -p "$dest"

fetch() {
    local file="$1" sha="$2"
    if [ -f "$dest/$file" ] && echo "$sha  $dest/$file" | sha256sum --check --status; then
        return
    fi
    curl --silent --show-error --fail --location --retry 2 --output "$dest/$file" "$base/$file"
    echo "$sha  $dest/$file" | sha256sum --check --status || {
        echo "$file failed its pinned SHA-256 check" >&2
        rm -f "$dest/$file"
        exit 1
    }
}

fetch 240pee.nes 04f01d7372f66ea2befe325b9bd655a9fcc395a31fa46d9466286bb8f9d2e62e
fetch gb240p.gb 733eb6e7ec0719752ddd5fcda4e715c2f2e20f4ac4bc971c4bd1d49272f9bb04
fetch 240pee_mb.gba 47844f7140738a06f8f3bc09780da3ab095539a250b870f614feed561d9d6f34

# Mega Drive sample ROMs of SGDK (Stephane Dallongeville, MIT; the ROMs include the MIT SGDK runtime), prebuilt in
# the repository at a pinned commit; used only by the experimental Genesis cores (experimental/).
sgdk="https://raw.githubusercontent.com/Stephane-D/SGDK/ee6870a6010b1e341ca36078f581a68a26c9a543/sample"
fetch_url() {
    local file="$1" sha="$2" url="$3"
    if [ -f "$dest/$file" ] && echo "$sha  $dest/$file" | sha256sum --check --status; then
        return
    fi
    curl --silent --show-error --fail --location --retry 2 --output "$dest/$file" "$url"
    echo "$sha  $dest/$file" | sha256sum --check --status || {
        echo "$file failed its pinned SHA-256 check" >&2
        rm -f "$dest/$file"
        exit 1
    }
}
fetch_url sgdk_platformer.md 12822e16a1dae6fcea5f82ebaba57d278e863c8efeae7ab5faefdaa094d3c792 "$sgdk/game/platformer/out/release/rom.bin"
fetch_url sgdk_benchmark.md 6f9af12f09911f87ed44b8ca31f587a691428b681586d3298b6578315712285d "$sgdk/benchmark/out/release/rom.bin"
python3 "$(dirname "$0")/make_test_roms.py" "$dest"
# md_music.md is generated (no third-party code or music); the pin catches an accidental change of the generator.
echo "14947d7854b6c0d7f66a6021162703eab772897a7a7417d61b6253a6f019940b  $dest/md_music.md" | sha256sum --check --status || {
    echo "md_music.md differs from its pinned SHA-256 (update the pin if tools/make_test_roms.py changed on purpose)" >&2
    exit 1
}
