#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cc -std=c99 -Wall -Wextra -Werror -pedantic -O2 \
  "$repo/tools/probe-ng-fb.c" -o /tmp/probe-ng-fb
if /tmp/probe-ng-fb /dev/not-a-framebuffer >/tmp/probe-ng-fb-bad 2>&1; then
  echo 'Read-only framebuffer probe accepted an unexpected device path' >&2
  exit 1
fi
grep -Fq 'Specify one /dev/fbN path only' /tmp/probe-ng-fb-bad
# An absent candidate is an expected platform capability, not a test failure.
echo 'PASS: fbdev read-only capability probe typechecks and restricts paths'
