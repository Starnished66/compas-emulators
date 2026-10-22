#!/usr/bin/env bash
set -euo pipefail

tool_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$tool_dir/../.." && pwd)
work=${R1_VM_WORK:-"$repo_root/build/r1-vm"}
force_images=0

while (($#)); do
    case "$1" in
        --force-images) force_images=1 ;;
        -h|--help)
            cat <<'EOF'
Usage: tools/r1-vm/build.sh [--force-images]

Build the host QEMU support files, Malta kernel, static BusyBox, and VM disks.
Existing disk images are preserved unless --force-images is supplied.
Set R1_VM_WORK to place all generated VM artifacts in another directory.
EOF
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

mkdir -p "$work"
work=$(cd "$work" && pwd)
export R1_VM_WORK="$work"
if [[ -n ${QEMU_SYSTEM_MIPSEL:-} ]]; then
    [[ -x "$QEMU_SYSTEM_MIPSEL" ]] || {
        echo "QEMU_SYSTEM_MIPSEL is not executable: $QEMU_SYSTEM_MIPSEL" >&2
        exit 2
    }
elif ! command -v qemu-system-mipsel >/dev/null 2>&1 &&
     [[ ! -x "$work/host/usr/bin/qemu-system-mipsel" ]]; then
    "$tool_dir/setup-host.sh"
fi
command -v qemu-mipsel >/dev/null 2>&1 || {
    echo "qemu-mipsel user emulator is required to prepare BusyBox applet links." >&2
    exit 2
}
"$tool_dir/build-kernel.sh"
"$tool_dir/build-busybox.sh"

image_args=(--work "$work")
if ((force_images)); then
    image_args+=(--force)
fi
python3 "$tool_dir/build-images.py" "${image_args[@]}"

echo "R1 VM build artifacts are in: $work"
echo "Start with: python3 '$tool_dir/vm.py' start"
