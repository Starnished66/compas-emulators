#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
work=${R1_VM_WORK:-"$repo/build/r1-vm"}
if command -v qemu-system-mipsel >/dev/null; then
    qemu-system-mipsel --version
    exit 0
fi
command -v dnf >/dev/null || { echo 'Install qemu-system-mipsel, or set QEMU_SYSTEM_MIPSEL to an existing binary.' >&2; exit 1; }
mkdir -p "$work/downloads" "$work/host"
# Fedora packages are verified and unpacked locally; no sudo/system install.
dnf download --arch=x86_64 --destdir "$work/downloads" \
    qemu-system-mips-core igvm-libs librdmacm qatzip-libs libslirp libxdp qatlib
for package in "$work"/downloads/*.x86_64.rpm; do
    rpmkeys --checksig "$package"
    rpm2cpio "$package" | (cd "$work/host" && cpio -idmu --quiet)
done
LD_LIBRARY_PATH="$work/host/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$work/host/usr/bin/qemu-system-mipsel" --version
