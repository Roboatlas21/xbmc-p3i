#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I./src/input \
  tests/test_cec_port.c -o "$out"
"$out"
