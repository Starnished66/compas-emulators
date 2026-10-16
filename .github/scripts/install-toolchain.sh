#!/usr/bin/env bash
# Installs the mipsel musl cross toolchain (GCC 11.2.1) used by Compas
# builds, from the same source and with the same pinned SHA-256 as
# compas-player's scripts/install_mips_toolchain.sh. Prints the bin folder.
set -euo pipefail

readonly mirror_url="https://github.com/musl-cc/musl.cc/releases/download/v0.0.1/mipsel-linux-musl-cross.tgz"
readonly fallback_url="https://musl.cc/mipsel-linux-musl-cross.tgz"
readonly sha256="82626533bf7e677c225e7cbedf1d5b0d6bc60c3daaf28249e54f0eb805d89b13"
readonly destination_root="${1:?usage: $0 DESTINATION_ROOT}"

mkdir -p "$destination_root"
download_dir=$(mktemp -d "${TMPDIR:-/tmp}/toolchain.XXXXXX")
trap 'rm -rf "$download_dir"' EXIT
archive="$download_dir/mipsel-linux-musl-cross.tgz"
if ! curl --silent --show-error --fail --location --retry 2 --retry-max-time 75 --connect-timeout 15 --max-time 120 \
    --output "$archive" "$mirror_url"; then
    echo "GitHub release mirror unavailable; trying musl.cc" >&2
    curl --silent --show-error --fail --location --retry 1 --retry-max-time 40 --connect-timeout 15 --max-time 120 \
        --output "$archive" "$fallback_url"
fi
echo "$sha256  $archive" | sha256sum --check --status || {
    echo "Downloaded toolchain failed its pinned SHA-256 check" >&2
    exit 1
}
tar -xzf "$archive" -C "$destination_root"
test -x "$destination_root/mipsel-linux-musl-cross/bin/mipsel-linux-musl-gcc" || {
    echo "Toolchain compiler was not extracted correctly." >&2
    exit 1
}
echo "$destination_root/mipsel-linux-musl-cross/bin"
