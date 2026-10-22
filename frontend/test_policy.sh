#!/bin/sh
set -eu
cc=${CC:-cc}
out=$(mktemp "${TMPDIR:-/tmp}/compas-policy.XXXXXX")
trap 'rm -f "$out"' EXIT HUP INT TERM
"$cc" -std=c11 -Wall -Wextra -Werror -I"$(dirname "$0")" \
    "$(dirname "$0")/policy_selftest.c" \
    "$(dirname "$0")/audio_policy.c" \
    "$(dirname "$0")/residency.c" -o "$out"
"$out"
