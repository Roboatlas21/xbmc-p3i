#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"${CC:-cc}" -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
  -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/libgamestream/xml.c" "$root/tests/test_native_xml_applist.c" \
  -lexpat -o "$tmp/test-native-xml"
ASAN_OPTIONS=detect_leaks=1 "$tmp/test-native-xml"
