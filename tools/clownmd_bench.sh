#!/usr/bin/env bash
# Times the ClownMDEmu variants (build/target/clownmd-variants/, see VARIANTS.txt
# there) with --bench on the test ROMs. Round robin over the variants, best of
# REPS runs each, because a loaded host or a device clock governor makes single
# runs noisy.
#
#   tools/clownmd_bench.sh [variant ...]     all variants when none is named
#
# Environment: RUNNER (default qemu-mipsel; set it to empty on the device),
# DIR (variant folder), ROMS (ROM folder), FRAMES (default 600), REPS (default 3).
# A line of the variant's own command line options comes from VARIANTS.txt:
# "name | description | extra arguments".
set -uo pipefail

dir="${DIR:-build/target/clownmd-variants}"
roms="${ROMS:-tests/roms}"
frames="${FRAMES:-600}"
reps="${REPS:-3}"
runner="${RUNNER-qemu-mipsel}"
list="$dir/VARIANTS.txt"

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
    while IFS='|' read -r name _; do
        name="${name// /}"
        [ -n "$name" ] && names+=("$name")
    done < "$list"
fi

args_for() {
    awk -F'|' -v n="$1" '{ gsub(/ /, "", $1); if ($1 == n) { sub(/^ +/, "", $3); print $3 } }' "$list"
}

declare -A best out
for rep in $(seq "$reps"); do
    for name in "${names[@]}"; do
        for rom in sgdk_platformer sgdk_benchmark; do
            # shellcheck disable=SC2046
            res=$($runner "$dir/$name" "$roms/$rom.md" --bench "$frames" $(args_for "$name") 2>&1)
            wall=$(sed -n 's/^frames [0-9]* *wall \([0-9.]*\)s.*/\1/p' <<<"$res")
            [ -z "$wall" ] && continue
            key="$name/$rom"
            if [ -z "${best[$key]:-}" ] || [ "$(bc -l <<<"$wall < ${best[$key]}")" = 1 ]; then
                best[$key]=$wall
                out[$key]="$(sed -n 's/^rendered \([0-9]*\) of \([0-9]*\) frames (video skipped on \([0-9]*\)).*/\1 \2 \3/p' <<<"$res") $(sed -n 's/^output check: audio \([0-9a-f]*\) video \([0-9a-f]*\)/\1 \2/p' <<<"$res")"
            fi
        done
    done
done

printf '%-26s %-16s %8s %9s %-14s %s\n' variant rom fps ms/frame "rendered/total" "audio/video hash"
for name in "${names[@]}"; do
    for rom in sgdk_platformer sgdk_benchmark; do
        key="$name/$rom"
        [ -z "${best[$key]:-}" ] && continue
        read -r r t _ ah vh <<<"${out[$key]}"
        printf '%-26s %-16s %8.1f %9.2f %-14s %s\n' "$name" "$rom" "$(bc -l <<<"$frames / ${best[$key]}")" \
            "$(bc -l <<<"${best[$key]} * 1000 / $frames")" "${r:-?}/${t:-?}" "${ah:--}/${vh:--}"
    done
done
