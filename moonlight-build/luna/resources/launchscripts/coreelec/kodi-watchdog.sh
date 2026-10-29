#!/bin/sh
# Restart Kodi if an independent Luna NG systemd session disappears after
# stopping kodi.service. This service is deliberately outside Kodi and outside
# the Luna session unit.
set -u
[ "$#" -ge 2 ] && [ "$#" -le 7 ] || exit 2
session_unit="$1"
session="$2"
stream_ready="$session.stream-ready"
governor_state="${3:-}"
network_state="${4:-}"
tsync_state="${5:-}"
hdmi_state="${6:-}"
aml_state="${7:-}"
scripts="$(CDPATH='' cd "$(dirname "$0")" && pwd)" || exit 1
. "$scripts/tsync.sh" || exit 126
. "$scripts/aml-recovery.sh" || exit 126
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
  rm -f "$session.ready" "$stream_ready"
  exit 0
}

# Hold the same kernel flock as normal cleanup. Never delete a live owner's lock.
recovery_lock="$session.recovery.lock"
exec 9>"$recovery_lock" || exit 1
# Do not steal a live kernel lock. If recovery is stuck, request systemd
# to stop the session unit; systemd owns bounded TERM/KILL escalation.
echo 'Luna NG watchdog: waiting for recovery ownership' >&2
if ! flock -w 30 9; then
  echo 'Luna NG watchdog: recovery stalled; requesting session termination' >&2
  "$SYSTEMCTL" stop --no-block "$session_unit" 2>/dev/null || true
  # The session unit has TimeoutStopSec=30s and KillMode=control-group.
  # The kernel releases its lock once the recovery owner actually exits.
  flock -w 45 9 || {
    echo 'Luna NG watchdog: recovery owner remains alive; SSH intervention required' >&2
    timeout -k 2 12 sh "$scripts/hang-diagnostics.sh" "$session_unit" "$session" >&2 || true
    exit 1
  }
fi
# The normal owner may have completed recovery while we waited.
[ -e "$session" ] || { flock -u 9; exit 0; }
armed="$(cat "$session" 2>/dev/null || printf '0')"
if [ "$armed" = 1 ]; then
  if [ -n "$hdmi_state" ]; then
    python3 "$scripts/../../lib/util/hdmigamemode.py" restore "$hdmi_state" || true
  fi
  # Recover only the exact recorded process, never a PID reused by the kernel.
  identity="$session.moonlight-pid"
  terminate_recorded_moonlight() {
    [ -r "$identity" ] || return 0
    IFS='|' read -r pid started expected < "$identity" || return 0
    case "$pid:$started" in *[!0-9:]*|:*|*:) return 0 ;; esac
    [ -r "/proc/$pid/stat" ] || return 0
    current_start="$(sed 's/^.*) //' "/proc/$pid/stat" | awk '{print $20}')"
    current_exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
    [ "$current_start" = "$started" ] && [ "$current_exe" = "$expected" ] || return 0
    kill -TERM "$pid" 2>/dev/null || true
    sleep 1
    current_start="$(sed 's/^.*) //' "/proc/$pid/stat" 2>/dev/null | awk '{print $20}')"
    current_exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
    if [ "$current_start" = "$started" ] && [ "$current_exe" = "$expected" ]; then
      kill -KILL "$pid" 2>/dev/null || true
    fi
  }
  terminate_recorded_moonlight
  # Exact cgroup membership is secondary to the recorded PID identity.
  # Re-check membership and executable before each signal; never kill by name alone.
  control_group="$("$SYSTEMCTL" show --property=ControlGroup --value "$session_unit" 2>/dev/null || true)"
  case "$control_group" in
    /*)
      cgroup_moonlight_pids() {
        for pidfile in /proc/[0-9]*/cgroup; do
          [ -r "$pidfile" ] || continue
          pid="${pidfile#/proc/}"; pid="${pid%/cgroup}"
          awk -F: -v group="$control_group" '$3 == group {found=1} END {exit !found}' "$pidfile" 2>/dev/null || continue
          exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
          case "$exe" in */moonlight) printf '%s\n' "$pid" ;; esac
        done
      }
      # Capture start times before TERM to protect against PID reuse.
      candidates=""
      for pid in $(cgroup_moonlight_pids); do
        started="$(sed 's/^.*) //' "/proc/$pid/stat" 2>/dev/null | awk '{print $20}')"
        [ -n "$started" ] || continue
        candidates="${candidates}${pid}|${started} "
        kill -TERM "$pid" 2>/dev/null || true
      done
      [ -z "$candidates" ] || sleep 2
      for item in $candidates; do
        pid="${item%%|*}"; started="${item#*|}"
        now="$(sed 's/^.*) //' "/proc/$pid/stat" 2>/dev/null | awk '{print $20}')"
        [ "$now" = "$started" ] || continue
        awk -F: -v group="$control_group" '$3 == group {found=1} END {exit !found}' "/proc/$pid/cgroup" 2>/dev/null || continue
        exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
        case "$exe" in */moonlight) kill -KILL "$pid" 2>/dev/null || true ;; esac
      done
      ;;
  esac
  rm -f "$identity"
  restore_tsync
  aml_restore "$aml_state"
  restore_network_tuning
  restore_cpu_governors
  aml_reset_video
  restore_kodi_framebuffers
  # Avoid restarting an already recovered Kodi instance.
  if "$SYSTEMCTL" is-active --quiet "$KODI_UNIT"; then
    printf '0\n' > "$session" 2>/dev/null || true
  fi
  attempt=0
  while ! "$SYSTEMCTL" is-active --quiet "$KODI_UNIT" && [ "$attempt" -lt 15 ]; do
    if "$SYSTEMCTL" start "$KODI_UNIT"; then
      printf '0\n' > "$session" 2>/dev/null || true
      break
    fi
    attempt=$((attempt + 1))
    sleep 2
  done
fi

rm -f "$session" "$session.ready" "$stream_ready" "$session.moonlight-pid"
[ -n "$aml_state" ] && rm -f "$aml_state"
[ -n "$governor_state" ] && rm -f "$governor_state"
[ -n "$network_state" ] && rm -f "$network_state"
[ -n "$tsync_state" ] && rm -f "$tsync_state"
[ -n "$hdmi_state" ] && { [ -s "$hdmi_state" ] || rm -f "$hdmi_state"; }
flock -u 9 2>/dev/null || true
exit 0
