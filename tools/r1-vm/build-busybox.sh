#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
vm_dir=${R1_VM_WORK:-"$repo_root/build/r1-vm"}
mkdir -p "$vm_dir"
vm_dir=$(cd "$vm_dir" && pwd)
download_dir="$vm_dir/downloads"
source_dir="$vm_dir/busybox-source"
build_dir="$vm_dir/busybox-build"
version=1.36.1
archive="busybox-${version}.tar.bz2"
url="https://busybox.net/downloads/${archive}"
mirror_url="https://deb.debian.org/debian/pool/main/b/busybox/busybox_${version}.orig.tar.bz2"
checksum=b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314
if [[ -n ${CROSS_COMPILE:-} ]]; then
    cross_prefix=$CROSS_COMPILE
else
    cross_prefix="$repo_root/../compas-player/toolchains/mipsel-linux-musl-cross/bin/mipsel-linux-musl-"
fi
if [[ ! -x "${cross_prefix}gcc" ]] && command -v mipsel-linux-musl-gcc >/dev/null 2>&1; then
    cross_prefix=$(command -v mipsel-linux-musl-gcc)
    cross_prefix=${cross_prefix%gcc}
fi
if [[ ! -x "${cross_prefix}gcc" ]]; then
    echo "MIPS little-endian cross compiler not found (set CROSS_COMPILE)." >&2
    exit 2
fi

mkdir -p "$download_dir" "$source_dir" "$build_dir"
archive_path="$download_dir/$archive"
if [[ ! -f "$archive_path" ]]; then
    temporary_archive="$archive_path.part"
    if ! curl --fail --location --connect-timeout 15 --max-time 90 --retry 2 "$url" --output "$temporary_archive"; then
        echo "BusyBox upstream download failed; trying Debian's source mirror." >&2
        rm -f "$temporary_archive"
        curl --fail --location --connect-timeout 15 --max-time 90 --retry 2 "$mirror_url" --output "$temporary_archive"
    fi
    printf '%s  %s\n' "$checksum" "$temporary_archive" | sha256sum --check --status || {
        rm -f "$temporary_archive"
        echo "BusyBox archive checksum mismatch: $temporary_archive" >&2
        exit 1
    }
    mv "$temporary_archive" "$archive_path"
fi
printf '%s  %s\n' "$checksum" "$archive_path" | sha256sum --check --status || {
    echo "BusyBox archive checksum mismatch: $archive_path" >&2
    exit 1
}

if [[ ! -f "$source_dir/Makefile" ]] || ! grep -q '^VERSION = 1$' "$source_dir/Makefile" || ! grep -q '^PATCHLEVEL = 36$' "$source_dir/Makefile" || ! grep -q '^SUBLEVEL = 1$' "$source_dir/Makefile"; then
    rm -rf "$source_dir"
    mkdir -p "$source_dir"
    tar -xjf "$archive_path" --strip-components=1 -C "$source_dir"
fi

make -C "$source_dir" O="$build_dir" CROSS_COMPILE="$cross_prefix" defconfig
# A static binary can be copied straight into the ext4 root filesystem without
# needing a dynamic loader or runtime shared libraries.
sed -i 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' "$build_dir/.config"
make -C "$source_dir" O="$build_dir" CROSS_COMPILE="$cross_prefix" oldconfig </dev/null
make -C "$source_dir" O="$build_dir" CROSS_COMPILE="$cross_prefix" -j"${JOBS:-6}"

install -D -m 0755 "$build_dir/busybox" "$vm_dir/busybox"
echo "Built static MIPS little-endian BusyBox: $vm_dir/busybox"
"${cross_prefix}readelf" -h "$vm_dir/busybox" | sed -n '1,18p'
file "$vm_dir/busybox"
