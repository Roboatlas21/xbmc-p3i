#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
good="$tmp/good"
missing="$tmp/not-yet-present"
export LUNA_AML_LOW_LATENCY="$good" LUNA_AML_SKIP_POLICY="$missing"
. "$root/resources/launchscripts/coreelec/aml-recovery.sh"
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
# Inject an ENOSPC-like journal append failure after mktemp has succeeded.
# The original recovery record must survive byte-for-byte.
rm -f "$missing"
printf '%s|2|0\n' "$missing" > "$state"
expected_journal="$(/bin/cat "$state")"
(
  printf() {
    case "$1" in
      '%s|%s|%s\n') return 1 ;;
      *) command printf "$@" ;;
    esac
  }
  if aml_restore "$state"; then
    echo 'Journal write failure incorrectly reported success' >&2
    exit 1
  fi
)
[[ "$(/bin/cat "$state")" == "$expected_journal" ]]
printf '0' > "$missing"
aml_restore "$state"
[[ "$(/bin/cat "$missing")" == 2 ]]
[[ ! -e "$state" ]]
# A torn final write is not a complete record, even with three fields.
printf '1' > "$good"
printf '%s|0|1\n%s|0|1' "$good" "$good" > "$state"
expected_journal="$(/bin/cat "$state")"
if aml_restore "$state" 2>"$tmp/truncated.err"; then
  echo 'Truncated AML snapshot incorrectly reported success' >&2
  exit 1
fi
grep -Fq 'truncated journal record' "$tmp/truncated.err"
[[ "$(/bin/cat "$state")" == "$expected_journal" ]]
printf '%s|0|1\n%s|0|1\n' "$good" "$good" > "$state"
aml_restore "$state"
[[ "$(/bin/cat "$good")" == 0 ]]
[[ ! -e "$state" ]]

# Malformed records must never be ignored and deleted.
printf '%s|0|1\ninvalid|\n' "$good" > "$state"
expected_journal="$(/bin/cat "$state")"
if aml_restore "$state" 2>"$tmp/malformed.err"; then
  echo 'Malformed AML snapshot incorrectly reported success' >&2
  exit 1
fi
grep -Fq 'malformed journal record' "$tmp/malformed.err"
[[ "$(/bin/cat "$state")" == "$expected_journal" ]]
printf '%s|0|1|extra\n' "$good" > "$state"
if aml_restore "$state" 2>"$tmp/extra.err"; then
  echo 'Extra journal fields incorrectly reported success' >&2
  exit 1
fi
grep -Fq 'extra journal fields' "$tmp/extra.err"
[[ -e "$state" ]]
# A forged recovery record cannot write another file even if its values look valid.
outside="$tmp/unrelated"
printf 1 > "$outside"
printf '%s|0|1\n' "$outside" > "$state"
if aml_restore "$state" 2>"$tmp/unsafe.err"; then
  echo 'Forged AML restore target was accepted' >&2
  exit 1
fi
grep -Fq 'unsafe recovery path' "$tmp/unsafe.err"
[[ "$(/bin/cat "$outside")" == 1 ]]
[[ -f "$state" ]]
echo 'PASS: AML recovery preserves failed, truncated, malformed and unsafe records'
