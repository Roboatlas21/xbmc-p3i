#!/bin/sh
# Restart Kodi if an independent Luna NG systemd session disappears after
# stopping kodi.service. This service is deliberately outside Kodi and outside
# the Luna session unit.
set -u
[ "$#" -ge 2 ] && [ "$#" -le 6 ] || exit 2
session_unit="$1"
session="$2"
governor_state="${3:-}"
network_state="${4:-}"
tsync_state="${5:-}"
hdmi_state="${6:-}"
scripts="$(CDPATH='' cd "$(dirname "$0")" && pwd)" || exit 1
. "$scripts/tsync.sh" || exit 126
SYSTEMCTL="${LUNA_SYSTEMCTL:-systemctl}"
KODI_UNIT="${LUNA_KODI_UNIT:-kodi.service}"

case "$session_unit" in
  ''|*[!A-Za-z0-9_.@-]*) exit 2 ;;
esac
[ -r "$session" ] || exit 1

restore_network_tuning() {
  [ -n "$network_state" ] && [ -s "$network_state" ] || return 0
  while IFS='|' read -r path original target; do
    [ -n "$path" ] && [ -n "$original" ] && [ -n "$target" ] || continue
    [ -r "$path" ] && [ -w "$path" ] || continue
    current="$(cat "$path" 2>/dev/null)" || current=''
    [ "$current" = "$target" ] || continue
    printf '%s' "$original" > "$path" 2>/dev/null || true
  done < "$network_state"
  rm -f "$network_state"
}

restore_tsync() {
  [ -n "$tsync_state" ] && [ -s "$tsync_state" ] || return 0
  IFS='|' read -r path original applied < "$tsync_state" || true
  if [ -n "${path:-}" ] && [ -n "${original:-}" ] && [ -n "${applied:-}" ] &&
     [ -r "$path" ] && [ -w "$path" ]; then
    current="$(read_tsync "$path")" || current=''
    [ "$current" = "$applied" ] &&
      printf '%s' "$original" > "$path" 2>/dev/null || true
  fi
  rm -f "$tsync_state"
}

restore_kodi_framebuffers() {
  for blank in "${LUNA_FB0_BLANK:-/sys/class/graphics/fb0/blank}" \
               "${LUNA_FB1_BLANK:-/sys/class/graphics/fb1/blank}"; do
    [ -e "$blank" ] || continue
    [ -w "$blank" ] && printf '0' > "$blank" 2>/dev/null || true
  done
}

restore_cpu_governors() {
  [ -n "$governor_state" ] && [ -s "$governor_state" ] || return 0
  while IFS='|' read -r governor original; do
    [ -n "$governor" ] && [ -n "$original" ] || continue
    [ -r "$governor" ] && [ -w "$governor" ] || continue
    current="$(cat "$governor" 2>/dev/null)" || current=''
    [ "$current" = performance ] || continue
    printf '%s' "$original" > "$governor" 2>/dev/null || true
  done < "$governor_state"
  rm -f "$governor_state"
}

printf '%s\n' ready > "$session.ready" || exit 1

while [ -e "$session" ]; do
  if ! "$SYSTEMCTL" is-active --quiet "$session_unit"; then
    break
  fi
  sleep 1
done

# Normal cleanup removes/disarms the marker before the session unit exits.
[ -e "$session" ] || {
  rm -f "$session.ready"
  exit 0
}

armed="$(cat "$session" 2>/dev/null || printf '0')"
if [ "$armed" = 1 ]; then
  if [ -n "$hdmi_state" ]; then
    python3 "$scripts/../../lib/util/hdmigamemode.py" restore "$hdmi_state" || true
  fi
  restore_tsync
  restore_network_tuning
  restore_cpu_governors
  restore_kodi_framebuffers
  attempt=0
  while [ "$attempt" -lt 15 ]; do
    if "$SYSTEMCTL" start "$KODI_UNIT"; then
      printf '0\n' > "$session" 2>/dev/null || true
      break
    fi
    attempt=$((attempt + 1))
    sleep 2
  done
fi

rm -f "$session" "$session.ready"
[ -n "$governor_state" ] && rm -f "$governor_state"
[ -n "$network_state" ] && rm -f "$network_state"
[ -n "$tsync_state" ] && rm -f "$tsync_state"
[ -n "$hdmi_state" ] && { [ -s "$hdmi_state" ] || rm -f "$hdmi_state"; }
exit 0
