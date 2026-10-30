#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
cc -std=gnu99 -Wall -Wextra -Werror -I./src/input \
  tests/test_mapping_override.c src/input/mapping.c -o "$out"
"$out"
