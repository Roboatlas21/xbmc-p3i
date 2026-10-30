#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
  -Itests/stubs tests/test_pulse_init.c src/audio/pulse_mapping.c -o "$out"
"$out"
