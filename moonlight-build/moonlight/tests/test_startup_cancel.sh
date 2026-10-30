#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
cc -std=gnu99 -Wall -Wextra -Werror -pthread \
  tests/test_startup_cancel.c src/startup_cancel.c -o "$out"
"$out"
