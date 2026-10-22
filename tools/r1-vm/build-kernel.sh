#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
vm_dir=${R1_VM_WORK:-"$repo_root/build/r1-vm"}
mkdir -p "$vm_dir"
vm_dir=$(cd "$vm_dir" && pwd)
download_dir="$vm_dir/downloads"
source_dir="$vm_dir/kernel-source"
build_dir="$vm_dir/kernel-build"
version=${KERNEL_VERSION:-4.4.94}
archive="linux-${version}.tar.xz"
url="https://cdn.kernel.org/pub/linux/kernel/v4.x/${archive}"
case "$version" in
    4.4.94) checksum=b4e8fbd0de4321e88092eeb7fc66ee61ce6e13c7ba6680d24fdd20f532b4f110 ;;
    4.4.302) checksum=35017bb40b604e0b577fc2b87e727632b46608a2ba3a4f5858b9177f58f376b3 ;;
    *) echo "Unsupported pinned kernel version: $version" >&2; exit 2 ;;
esac
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
    curl --fail --location --retry 3 "$url" --output "$archive_path"
fi
printf '%s  %s\n' "$checksum" "$archive_path" | sha256sum --check --status || {
    echo "Kernel archive checksum mismatch: $archive_path" >&2
    exit 1
}

if [[ ! -f "$source_dir/Makefile" ]] ||
   ! grep -q '^VERSION = 4$' "$source_dir/Makefile" ||
   ! grep -q '^PATCHLEVEL = 4$' "$source_dir/Makefile" ||
   ! grep -q "^SUBLEVEL = ${version##*.}$" "$source_dir/Makefile"; then
    rm -rf "$source_dir"
    mkdir -p "$source_dir"
    tar -xJf "$archive_path" --strip-components=1 -C "$source_dir"
fi

make -C "$source_dir" O="$build_dir" ARCH=mips CROSS_COMPILE="$cross_prefix" malta_defconfig
"$source_dir/scripts/kconfig/merge_config.sh" -m -O "$build_dir" \
    "$build_dir/.config" "$repo_root/tools/r1-vm/malta.config"
make -C "$source_dir" O="$build_dir" ARCH=mips CROSS_COMPILE="$cross_prefix" \
    HOSTCFLAGS="-O2 -fcommon" olddefconfig
make -C "$source_dir" O="$build_dir" ARCH=mips CROSS_COMPILE="$cross_prefix" \
    HOSTCFLAGS="-O2 -fcommon" KCFLAGS="-Wno-error=attribute-alias" \
    -j"${JOBS:-6}" vmlinux

cp "$build_dir/vmlinux" "$vm_dir/vmlinux"
cp "$build_dir/.config" "$vm_dir/kernel.config"
echo "Built Malta kernel: $vm_dir/vmlinux"
"${cross_prefix}readelf" -h "$vm_dir/vmlinux" | sed -n '1,18p'
