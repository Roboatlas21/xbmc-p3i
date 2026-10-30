#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
cc -std=c99 -Wall -Wextra -Werror tests/test_aml_decode_unit_validate.c -o "$out"
"$out"
