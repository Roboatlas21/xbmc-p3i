#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
. "$root/resources/launchscripts/coreelec/aml-recovery.sh"
good="$tmp/good"
missing="$tmp/not-yet-present"
state="$tmp/aml-state"
printf '1' > "$good"
printf '%s|0|1\n' "$good" > "$state"
printf '%s|2|0\n' "$missing" >> "$state"
if aml_restore "$state"; then
  echo 'Incomplete AML restoration incorrectly succeeded' >&2
  exit 1
fi
[[ "$(/bin/cat "$good")" == 0 ]]
[[ -f "$state" ]]
[[ "$(wc -l < "$state")" == 1 ]]
grep -Fq "$missing|2|0" "$state"
printf '0' > "$missing"
aml_restore "$state"
[[ "$(/bin/cat "$missing")" == 2 ]]
[[ ! -e "$state" ]]
# Externally modified values must never be overwritten or retried.
printf '9' > "$good"
printf '%s|0|1\n' "$good" > "$state"
aml_restore "$state"
[[ "$(/bin/cat "$good")" == 9 ]]
[[ ! -e "$state" ]]
echo 'PASS: AML recovery preserves only failed entries and verifies retries'
