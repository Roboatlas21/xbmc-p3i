#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
cc -std=gnu99 -Wall -Wextra -Wno-unused-parameter -pthread \
  -I./src -I./third_party/moonlight-common-c/src \
  tests/test_loop_signal_mask.c -o "$out"
"$out"
