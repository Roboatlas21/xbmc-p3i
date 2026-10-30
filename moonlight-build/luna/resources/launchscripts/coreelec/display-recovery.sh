#!/bin/sh
# Display recovery with a write-ahead journal, always invoked with timeout.
set -u
scripts="$(CDPATH='' cd "$(dirname "$0")" && pwd)" || exit 1
. "$scripts/kodi-state.sh" || exit 126
if [ "${1:-}" = --verify ]; then
  [ "$#" -eq 4 ] || exit 2
  record="$2"; fb0="$3"; fb1="$4"
  [ -s "$record" ] || exit 1
  pending="$(cat "$record" 2>/dev/null)" || exit 1
  # Only known framebuffer failures can be verified after Kodi is active.
  # A timeout, decoder issue or video write failure needs Kodi inactive.
  case "$pending" in fb0|fb1|fb0_fb1) ;; *) exit 1 ;; esac
  [ -e "$fb0" ] && [ -r "$fb0" ] || exit 1
  [ "$(cat "$fb0" 2>/dev/null)" = 0 ] || exit 1
  if [ -e "$fb1" ]; then
    [ -r "$fb1" ] && [ "$(cat "$fb1" 2>/dev/null)" = 0 ] || exit 1
  else
    case "$pending" in fb1|fb0_fb1) exit 1 ;; esac
  fi
  exit 0
fi
record=''
kodi_guard=''
if [ "${1:-}" = --restore ]; then
  [ "$#" -eq 6 ] || [ "$#" -eq 7 ] || exit 2
  record="$2"
  if [ "$#" -eq 7 ]; then
    kodi_guard="$7"
    set -- "$3" "$4" "$5" "$6"
  else
    shift 2
  fi
  printf '%s\n' unverified > "$record" || exit 1
fi
[ "$#" -eq 4 ] || exit 2
video="$1"; status="$2"; fb0="$3"; fb1="$4"
# Called with a trusted service name by Luna's launcher and watchdog.
# Re-check before every output write; an earlier Kodi-state observation can
# become stale while audio, sysfs and HDMI restoration is in progress.
kodi_still_inactive() {
  [ -n "$kodi_guard" ] || return 0
  case "$kodi_guard" in *[!A-Za-z0-9_.@-]*|'') return 1 ;; esac
  kg_state="$(luna_timeout -k 2 5 "${LUNA_SYSTEMCTL:-systemctl}" show \
      --property=ActiveState --value "$kodi_guard" 8>&- 9>&- 2>/dev/null)" || return 1
  case "$kg_state" in inactive|failed) return 0 ;; *) return 1 ;; esac
}
failed=0; critical=0; fb0_failed=0; fb1_failed=0
# On CoreELEC NG both the video-plane switch and fb0 are required for
# returning display ownership to Kodi. Missing controls must NEVER be
# considered a successful restore, even if an optional fb1 is unavailable.
if [ ! -e "$video" ] || [ ! -r "$video" ] || [ ! -w "$video" ]; then
  echo "Luna NG: required video-plane control is unavailable: $video" >&2
  exit 1
fi
if [ ! -e "$fb0" ] || [ ! -r "$fb0" ] || [ ! -w "$fb0" ]; then
  echo "Luna NG: required Kodi primary framebuffer is unavailable: $fb0" >&2
  exit 1
fi
if ! kodi_still_inactive; then
  echo 'Luna NG: Kodi is active/unknown; refusing video-plane recovery writes' >&2
  exit 1
fi
if ! printf '1' > "$video" 2>/dev/null ||
   [ "$(cat "$video" 2>/dev/null)" != 1 ]; then
  echo "Luna NG: video plane reset/readback failed: $video" >&2
  failed=1; critical=1
fi
if [ -r "$status" ]; then
  attempt=0
  while ! grep -Fq 'No vdec.' "$status" 2>/dev/null; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 3 ]; then
      echo "Luna NG: decoder still active after teardown" >&2
      failed=1; critical=1
      break
    fi
    sleep 1
  done
fi
for blank in "$fb0" "$fb1"; do
  [ -e "$blank" ] || continue
  if ! kodi_still_inactive; then
    echo 'Luna NG: Kodi took display ownership during recovery; stopping writes' >&2
    failed=1; critical=1
    break
  fi
  if [ ! -w "$blank" ] || ! printf '0' > "$blank" 2>/dev/null ||
     [ "$(cat "$blank" 2>/dev/null)" != 0 ]; then
    echo "Luna NG: framebuffer unblank/readback failed: $blank" >&2
    failed=1
    if [ "$blank" = "$fb0" ]; then fb0_failed=1; else fb1_failed=1; fi
  fi
done
if [ -n "$record" ]; then
  # Do not retire the journal if Kodi became active after the last write.
  if ! kodi_still_inactive; then
    failed=1; critical=1
  fi
  if [ "$failed" -eq 0 ]; then
    rm -f "$record" || exit 1
  else
    pending=unverified
    if [ "$critical" -eq 1 ]; then
      pending=critical
    elif [ "$fb0_failed" -eq 1 ] && [ "$fb1_failed" -eq 1 ]; then
      pending=fb0_fb1
    elif [ "$fb0_failed" -eq 1 ]; then
      pending=fb0
    elif [ "$fb1_failed" -eq 1 ]; then
      pending=fb1
    fi
    printf '%s\n' "$pending" > "$record" || exit 1
  fi
fi
exit "$failed"
