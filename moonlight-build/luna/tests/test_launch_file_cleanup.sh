#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin"
. "$root/resources/launchscripts/coreelec/session-ownership.sh"

session="$tmp/.luna-session.test"
config="$tmp/bin/.luna-stream-owned.conf"
ack="$tmp/bin/.luna-ack-owned.ack"
printf 'config' > "$config"
printf 'armed\n' > "$ack"
printf 'abandoned\n' > "$ack.abandoned"
printf '%s\n' "$tmp/bin" "$config" "$ack" > "$session.launch-files"
luna_cleanup_owned_launch_files "$session"
[ ! -e "$config" ] && [ ! -e "$ack" ] &&
[ ! -e "$ack.abandoned" ] && [ ! -e "$session.launch-files" ]

# A manipulated recovery marker must not erase an unrelated file.
important="$tmp/important"
printf 'preserve' > "$important"
printf '%s\n' "$tmp/bin" "$important" "$ack" > "$session.launch-files"
if luna_cleanup_owned_launch_files "$session"; then
  echo "unsafe recovery metadata was accepted" >&2; exit 1
fi
[ "$(cat "$important")" = preserve ]
[ -f "$session.launch-files" ]
# Traversal within a matching prefix must not delete an outside file.
mkdir -p "$tmp/bin/.luna-stream-dir.conf" "$tmp/bin/.luna-ack-dir.ack"
outer="$tmp/important.conf"
printf 'untouched' > "$outer"
evil="$tmp/bin/.luna-stream-dir.conf/../../important.conf"
printf '%s\n' "$tmp/bin" "$evil" "$ack" > "$session.launch-files"
if luna_cleanup_owned_launch_files "$session"; then
  echo "path traversal accepted by cleanup" >&2; exit 1
fi
[ "$(cat "$outer")" = untouched ]

# The normal launcher must reject equivalent paths before opening Kodi.
luna_owned_file_valid "$tmp/bin" "$tmp/bin/.luna-stream-good.conf" stream
! luna_owned_file_valid "$tmp/bin" "$evil" stream
! luna_owned_file_valid "$tmp/bin" "$tmp/bin/.luna-ack-dir.ack/../../important.ack" ack
! luna_owned_file_valid "$tmp/bin" "$tmp/bin/sub/.luna-stream-name.conf" stream

# File ownership metadata must not redirect cleanup to another directory.
printf '%s\n' "$tmp" "$outer" "" > "$session.launch-files"
export LUNA_NATIVE_BIN_DIR="$tmp/bin"
if luna_cleanup_owned_launch_files "$session"; then
  echo "outside binary directory accepted" >&2; exit 1
fi
unset LUNA_NATIVE_BIN_DIR
[ "$(cat "$outer")" = untouched ]
# Both current and older random-name journals stay within one session.
owned="$tmp/.luna-session.12345678"
printf '0\n' > "$owned"
for kind in governor network tsync hdmi aml; do
  : > "$owned.$kind-state"
done
printf '%s\n' "$owned.governor-state" "$owned.network-state" \
  "$owned.tsync-state" "$owned.hdmi-state" "$owned.aml-state" > "$owned.journals"
luna_load_session_journals "$owned"
[ "$LUNA_JOURNAL_GOV" = "$owned.governor-state" ]
luna_valid_session_journal "$owned" "$tmp/.luna-governor.ABC12345" governor
! luna_valid_session_journal "$owned" "$tmp/outside" network
! luna_valid_session_journal "$owned" "$tmp/.luna-network.ABC12345/../../outside" network
! luna_valid_session_journal "$owned" "$tmp/.luna-network.12345678/../outside" network
rm -f "$owned.network-state"
ln -s "$important" "$owned.network-state"
if luna_load_session_journals "$owned"; then
  echo 'Symlinked recovery journal was accepted' >&2; exit 1
fi
[ "$(cat "$important")" = preserve ]
rm -f "$owned.network-state"
: > "$owned.network-state"
printf '%s\n' "$owned.governor-state" "$owned.network-state" \
  "$owned.tsync-state" "$owned.hdmi-state" "$owned.aml-state" "$tmp/extra" > "$owned.journals"
! luna_load_session_journals "$owned"

# An updated/deleted add-on binary is still an exact native process.
luna_is_moonlight_executable "/addon/bin/moonlight (deleted)"
luna_native_executable_matches "/addon/bin/moonlight (deleted)" "/addon/bin/moonlight"
! luna_native_executable_matches "/other/bin/moonlight (deleted)" "/addon/bin/moonlight"
! luna_is_moonlight_executable "/addon/bin/moonlight-attacker (deleted)"

echo 'PASS: Luna-owned launch files and recovery journals remain within their owners'
