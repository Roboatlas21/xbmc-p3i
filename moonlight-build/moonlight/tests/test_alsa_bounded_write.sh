#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$root/src/audio" \
  "$root/tests/test_alsa_bounded_write.c" -o "$tmp/test-alsa-bounded-write"
"$tmp/test-alsa-bounded-write"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$root/src/audio" \
  "$root/tests/test_alsa_suspend_recovery.c" -o "$tmp/test-alsa-suspend-recovery"
"$tmp/test-alsa-suspend-recovery"
