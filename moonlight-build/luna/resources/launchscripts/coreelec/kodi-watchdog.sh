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
. "$scripts/recovery-journal.sh" || exit 126
. "$scripts/kodi-state.sh" || exit 126
. "$scripts/session-ownership.sh" || exit 126
SYSTEMCTL="${LUNA_SYSTEMCTL:-systemctl}"
KODI_UNIT="${LUNA_KODI_UNIT:-kodi.service}"
# Time limits apply only after recovery starts; long gaming sessions are
# unaffected. Every potentially blocking service/helper call is bounded.
RECOVERY_TIMEOUT="${LUNA_RECOVERY_TIMEOUT:-120}"
RECOVERY_HELPER_TIMEOUT="${LUNA_RECOVERY_HELPER_TIMEOUT:-15}"
for setting in RECOVERY_TIMEOUT RECOVERY_HELPER_TIMEOUT; do
  eval "value=\${$setting}"
  case "$value" in ''|*[!0-9]*) value='' ;; esac
  case "$setting:$value" in
    RECOVERY_TIMEOUT:|RECOVERY_TIMEOUT:0) RECOVERY_TIMEOUT=120 ;;
    RECOVERY_HELPER_TIMEOUT:|RECOVERY_HELPER_TIMEOUT:0) RECOVERY_HELPER_TIMEOUT=15 ;;
    RECOVERY_TIMEOUT:*) [ "$value" -le 300 ] || RECOVERY_TIMEOUT=120 ;;
    RECOVERY_HELPER_TIMEOUT:*) [ "$value" -le 60 ] || RECOVERY_HELPER_TIMEOUT=15 ;;
  esac
done
CLEANUP_TIMEOUT="${LUNA_CLEANUP_TIMEOUT:-45}"
case "$CLEANUP_TIMEOUT" in
  ''|*[!0-9]*) CLEANUP_TIMEOUT=45 ;;
esac
if [ "$CLEANUP_TIMEOUT" -lt 1 ] || [ "$CLEANUP_TIMEOUT" -gt 300 ]; then
  CLEANUP_TIMEOUT=45
fi
STARTUP_TIMEOUT="${LUNA_STARTUP_TIMEOUT:-20}"
case "$STARTUP_TIMEOUT" in ''|*[!0-9]*) STARTUP_TIMEOUT=20 ;; esac
if [ "$STARTUP_TIMEOUT" -lt 5 ] || [ "$STARTUP_TIMEOUT" -gt 120 ]; then
  STARTUP_TIMEOUT=20
fi

case "$session_unit" in
  ''|*[!A-Za-z0-9_.@-]*) exit 2 ;;
esac
# A queued Restart=on-failure may fire after successful session retirement.
[ -e "$session" ] || exit 0
[ -r "$session" ] || exit 1
if ! luna_valid_session_marker "$session"; then
  echo 'Luna NG watchdog: invalid session file path' >&2
  exit 1
fi
# Validate both detached watchdog arguments and mutable on-disk manifests.
if [ -e "$session.journals" ]; then
  if ! luna_load_session_journals "$session"; then
    echo 'Luna NG watchdog: unsafe recovery journal manifest' >&2
    exit 1
  fi
  if [ "$governor_state" != "$LUNA_JOURNAL_GOV" ] ||
     [ "$network_state" != "$LUNA_JOURNAL_NET" ] ||
     [ "$tsync_state" != "$LUNA_JOURNAL_TSYNC" ] ||
     [ "$hdmi_state" != "$LUNA_JOURNAL_HDMI" ] ||
     [ "$aml_state" != "$LUNA_JOURNAL_AML" ]; then
    echo 'Luna NG watchdog: journal arguments disagree with manifest' >&2
    exit 1
  fi
else
  [ -z "$governor_state" ] || luna_valid_session_journal "$session" "$governor_state" governor || exit 1
  [ -z "$network_state" ] || luna_valid_session_journal "$session" "$network_state" network || exit 1
  [ -z "$tsync_state" ] || luna_valid_session_journal "$session" "$tsync_state" tsync || exit 1
  [ -z "$hdmi_state" ] || luna_valid_session_journal "$session" "$hdmi_state" hdmi || exit 1
  [ -z "$aml_state" ] || luna_valid_session_journal "$session" "$aml_state" aml || exit 1
fi

restore_network_tuning() {
  [ -n "$network_state" ] || return 0
  restore_tuning_journal "$network_state" network
}

restore_tsync() {
  [ -n "$tsync_state" ] || return 0
  restore_tsync_snapshot "$tsync_state"
}

restore_cpu_governors() {
  [ -n "$governor_state" ] || return 0
  restore_tuning_journal "$governor_state" governor
}

# ActiveState is distinct from is-active's exit code: a deactivating unit is
# not stopped. Unknown responses and timeouts are never proof of termination.
read_session_state() {
  ss_state="$(timeout -k 2 5 "$SYSTEMCTL" show --property=ActiveState --value "$session_unit" 9>&- 2>/dev/null)" || return 1
  case "$ss_state" in
    active|activating|reloading|deactivating|inactive|failed)
      printf '%s\n' "$ss_state"; return 0 ;;
  esac
  return 1
}

# Never use a reused unit name as proof that the old launcher still owns it.
session_owner_matches() {
  [ -r "$session.owner" ] || return 1
  IFS='|' read -r owner_pid owner_start < "$session.owner" || return 1
  case "$owner_pid:$owner_start" in *[!0-9:]*|:*|*:) return 1 ;; esac
  [ -r "/proc/$owner_pid/stat" ] || return 1
  observed="$(sed 's/^.*) //' "/proc/$owner_pid/stat" 2>/dev/null | awk '{print $20}')" || return 1
  [ "$observed" = "$owner_start" ] || return 1
  unit_pid="$(timeout -k 2 5 "$SYSTEMCTL" show --property=MainPID --value "$session_unit" 9>&- 2>/dev/null)" || return 1
  [ "$unit_pid" = "$owner_pid" ]
}

# A no-block stop is asynchronous. Do not restore Kodi while the launcher
# still runs potentially blocked driver setup and has not taken its lock.
request_stop_and_wait() {
  if [ "${LUNA_RECOVER_ONLY:-0}" = 1 ]; then
    echo 'Luna NG watchdog: recover-only mode must not stop another service' >&2
    return 1
  fi
  stop_state="$(read_session_state || true)"
  case "$stop_state" in
    inactive|failed) return 0 ;;
    deactivating) : ;;
    active|activating|reloading)
      if ! session_owner_matches; then
        echo 'Luna NG watchdog: unit owner changed; refusing stop' >&2
        return 1
      fi
      if ! timeout -k 2 8 "$SYSTEMCTL" stop --no-block "$session_unit" 9>&- 2>/dev/null; then
        echo 'Luna NG watchdog: stop request unconfirmed; checking unit state' >&2
      fi ;;
    *) echo 'Luna NG watchdog: unit state unknown; refusing stop' >&2; return 1 ;;
  esac
  stop_deadline=$(( $(date +%s) + 40 ))
  while :; do
    ss_state="$(read_session_state || true)"
    case "$ss_state" in
      inactive|failed) return 0 ;;
      active|activating|reloading|deactivating|'') : ;;
    esac
    if [ "$(date +%s)" -ge "$stop_deadline" ]; then
      echo 'Luna NG watchdog: cannot confirm session terminated; refusing concurrent Kodi recovery' >&2
      return 1
    fi
    sleep 1
  done
}

# Recover-only is allowed only after the previous launcher has exited. Even
# direct invocations must not take its lock or restore displays while live.
if [ "${LUNA_RECOVER_ONLY:-0}" = 1 ]; then
  if ! luna_session_unit_quiescent "$session_unit" "$session"; then
    echo 'Luna NG watchdog: prior systemd session still owns resources' >&2
    exit 1
  fi
  [ -r "$session.owner" ] || {
    echo 'Luna NG watchdog: recover-only mode requires an old owner record' >&2
    exit 1
  }
  IFS='|' read -r prior_pid prior_start < "$session.owner" || exit 1
  case "$prior_pid:$prior_start" in *[!0-9:]*|:*|*:) exit 1 ;; esac
  if [ -r "/proc/$prior_pid/stat" ]; then
    prior_actual="$(sed 's/^.*) //' "/proc/$prior_pid/stat" 2>/dev/null | awk '{print $20}')"
    if [ "$prior_actual" = "$prior_start" ]; then
      echo 'Luna NG watchdog: previous launcher still alive; refusing recovery' >&2
      exit 1
    fi
  fi
fi

printf '%s\n' ready > "$session.ready" || exit 1

# Recover-only mode is used by a new launcher after validating the old
# process identity; the fixed unit name now refers to the NEW launcher.
if [ "${LUNA_RECOVER_ONLY:-0}" != 1 ]; then
while [ -e "$session" ]; do
  ss_state="$(read_session_state || true)"
  case "$ss_state" in
    inactive|failed) break ;;
    active|activating|reloading)
      if ! session_owner_matches; then
        echo 'Luna NG watchdog: unit is no longer owned by this session' >&2
        exit 1
      fi ;;
    deactivating) : ;;
    *) echo 'Luna NG watchdog: session state unknown; waiting without restoring Kodi' >&2 ;;
  esac
  # Before first stream readiness, bound the entire stopped-Kodi interval:
  # driver activation, VFM probing, and HDMI setup included.
  if [ -r "$session.startup" ] &&
     ! { [ -r "$stream_ready" ] && grep -Fxq ready "$stream_ready" 2>/dev/null; }; then
    started="$(cat "$session.startup" 2>/dev/null || true)"
    case "$started" in ''|*[!0-9]*) started='' ;; esac
    if [ -n "$started" ]; then
      now="$(date +%s)"
      if [ "$now" -ge "$started" ] &&
         [ "$((now - started))" -ge "$STARTUP_TIMEOUT" ]; then
        echo "Luna NG watchdog: startup exceeded ${STARTUP_TIMEOUT}s; stopping stuck session" >&2
        request_stop_and_wait || exit 1
        break
      fi
    fi
  fi
  # Do not limit legitimate game duration. Watch only a launcher that has
  # explicitly entered cleanup and failed to complete that phase.
  if [ -r "$session.cleanup" ]; then
    started="$(cat "$session.cleanup" 2>/dev/null || true)"
    case "$started" in
      ''|*[!0-9]*) started='' ;;
    esac
    if [ -n "$started" ]; then
      now="$(date +%s)"
      if [ "$now" -ge "$started" ] &&
         [ "$((now - started))" -ge "$CLEANUP_TIMEOUT" ]; then
        echo "Luna NG watchdog: cleanup exceeded ${CLEANUP_TIMEOUT}s, stopping stuck session" >&2
        request_stop_and_wait || exit 1
        break
      fi
    fi
  fi
  sleep 1
done
fi

# Normal cleanup removes/disarms the marker before the session unit exits.
[ -e "$session" ] || {
  rm -f "$session.ready" "$stream_ready" "$session.startup"
  exit 0
}

# Hold the same kernel flock as normal cleanup. Never delete a live owner's lock.
recovery_lock="$session.recovery.lock"
exec 9>"$recovery_lock" || exit 1
# Do not steal a live kernel lock. If recovery is stuck, request systemd
# to stop the session unit; systemd owns bounded TERM/KILL escalation.
echo 'Luna NG watchdog: waiting for recovery ownership' >&2
if ! flock -w 30 9; then
  if [ "${LUNA_RECOVER_ONLY:-0}" = 1 ]; then
    echo 'Luna NG watchdog: earlier recovery owner still holds the lock' >&2
    exit 1
  fi
  echo 'Luna NG watchdog: recovery stalled; requesting session termination' >&2
  request_stop_and_wait || exit 1
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
if [ "${LUNA_RECOVER_ONLY:-0}" = 1 ] &&
   ! luna_session_unit_quiescent "$session_unit" "$session"; then
  echo 'Luna NG watchdog: previous unit changed state while awaiting recovery lock' >&2
  flock -u 9 2>/dev/null || true
  exit 1
fi
recovery_started="$(date +%s)"
# Bound each restoration by its helper limit and the remaining whole-session
# recovery budget. An expired budget retains the relevant recovery journals.
run_recovery_step() {
  rs_now="$(date +%s)"
  rs_remaining=$((RECOVERY_TIMEOUT - (rs_now - recovery_started)))
  [ "$rs_remaining" -gt 0 ] || return 124
  rs_limit="$RECOVERY_HELPER_TIMEOUT"
  if [ "$rs_limit" -gt "$rs_remaining" ]; then
    rs_limit="$rs_remaining"
  fi
  timeout -k 2 "$rs_limit" "$@" 9>&-
}
# 'is-active' returning nonzero is NOT evidence that Kodi has stopped.
# A timed-out/unknown query must never authorize writes to Kodi-owned sysfs.
kodi_state="$(kodi_service_state)"

# A damaged arm marker cannot be treated as an unarmed session;
# otherwise kernel recovery journals could be silently discarded.
if luna_receipt_is "$session" 1; then
  armed=1
elif luna_receipt_is "$session" 0; then
  armed=0
else
  echo 'Luna NG watchdog: invalid session arm marker; retaining recovery state' >&2
  flock -u 9 2>/dev/null || true
  exit 1
fi
# Older armed sessions always stopped Kodi. New snapshots explicitly record
# whether Kodi was already inactive before Moonlight changed any resources.
kodi_initial=active
if [ -e "$session.kodi-initial" ]; then
  if luna_receipt_is "$session.kodi-initial" active; then
    kodi_initial=active
  elif luna_receipt_is "$session.kodi-initial" inactive; then
    kodi_initial=inactive
  else
    echo 'Luna NG watchdog: invalid initial Kodi state; retaining recovery' >&2
    flock -u 9 2>/dev/null || true
    exit 1
  fi
fi
if [ "$armed" = 1 ]; then
  # Recover only the exact recorded process, never a PID reused by the kernel.
  identity="$session.moonlight-pid"
  terminate_recorded_moonlight() {
    [ -r "$identity" ] || return 0
    IFS='|' read -r pid started expected < "$identity" || return 0
    case "$pid:$started" in *[!0-9:]*|:*|*:) return 0 ;; esac
    [ -r "/proc/$pid/stat" ] || return 0
    current_start="$(sed 's/^.*) //' "/proc/$pid/stat" | awk '{print $20}')"
    current_exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
    [ "$current_start" = "$started" ] &&
      luna_native_executable_matches "$current_exe" "$expected" || return 0
    kill -TERM "$pid" 2>/dev/null || true
    sleep 1
    current_start="$(sed 's/^.*) //' "/proc/$pid/stat" 2>/dev/null | awk '{print $20}')"
    current_exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
    if [ "$current_start" = "$started" ] &&
       luna_native_executable_matches "$current_exe" "$expected"; then
      kill -KILL "$pid" 2>/dev/null || true
    fi
  }
  terminate_recorded_moonlight
  # Exact cgroup membership is secondary to the recorded PID identity.
  # Re-check membership and executable before each signal; never kill by name alone.
  control_group=''
  if [ "${LUNA_RECOVER_ONLY:-0}" != 1 ]; then
    control_group="$(timeout -k 2 5 "$SYSTEMCTL" show --property=ControlGroup --value "$session_unit" 9>&- 2>/dev/null || true)"
  fi
  case "$control_group" in
    /*)
      cgroup_moonlight_pids() {
        for pidfile in /proc/[0-9]*/cgroup; do
          [ -r "$pidfile" ] || continue
          pid="${pidfile#/proc/}"; pid="${pid%/cgroup}"
          awk -F: -v group="$control_group" '$3 == group {found=1} END {exit !found}' "$pidfile" 2>/dev/null || continue
          exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
          if luna_is_moonlight_executable "$exe"; then
            printf '%s\n' "$pid"
          fi
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
        if luna_is_moonlight_executable "$exe"; then
          kill -KILL "$pid" 2>/dev/null || true
        fi
      done
      ;;
  esac
  # Check the exact native identity and all Moonlight processes remaining in
  # this session's cgroup BEFORE touching global codec/HDMI/FB state.
  if [ ! -e "$session.native-exited" ] &&
     [ -e "$session.native-launch-attempted" ] &&
     [ -r "$session.moonlight-pid" ] &&
     luna_recorded_native_gone "$session" &&
     luna_native_processes_stopped "$session" "$session_unit"; then
    luna_write_receipt "$session.native-exited" exited || exit 1
  fi
  if ! luna_native_processes_stopped "$session" "$session_unit"; then
    echo 'Luna NG watchdog: native process termination unverified; preserving recovery state' >&2
    flock -u 9 2>/dev/null || true
    exit 1
  fi
  hdmi_recovered=1
  if [ -n "$hdmi_state" ]; then
    if ! run_recovery_step python3 "$scripts/../../lib/util/hdmigamemode.py" restore "$hdmi_state"; then
      hdmi_recovered=0
      echo "Luna NG watchdog: HDMI restore timed out/failed; retaining state" >&2
    fi
  fi
  # Keep the recorded PID until the recovery marker is retired. Another
  # launcher must be able to detect any native process that survived TERM/KILL.
  tsync_recovered=1
  if ! run_recovery_step sh -c '. "$1"; restore_tsync_snapshot "$2"' luna-tsync \
       "$scripts/tsync.sh" "$tsync_state"; then
    tsync_recovered=0
    echo "Luna NG watchdog: tsync restore timed out/failed; retaining state" >&2
  fi
  aml_recovered=1
  if ! run_recovery_step sh -c '. "$1"; aml_restore "$2"' luna-aml \
       "$scripts/aml-recovery.sh" "$aml_state"; then
    aml_recovered=0
    echo "Luna NG watchdog: AML restoration timed out/failed; retaining recovery marker" >&2
  fi
  network_recovered=1
  if ! run_recovery_step sh -c '. "$1"; restore_tuning_journal "$2" network' luna-network \
       "$scripts/recovery-journal.sh" "$network_state"; then
    network_recovered=0
    echo "Luna NG watchdog: network restore timed out/failed; retaining journal" >&2
  fi
  governor_recovered=1
  if ! run_recovery_step sh -c '. "$1"; restore_tuning_journal "$2" governor' luna-governor \
       "$scripts/recovery-journal.sh" "$governor_state"; then
    governor_recovered=0
    echo "Luna NG watchdog: CPU governor restore timed out/failed; retaining journal" >&2
  fi
  # Native/HDMI/AML restoration may have taken many seconds. Re-read Kodi
  # state immediately before deciding whether display writes are permitted.
  kodi_state="$(kodi_service_state)"
  display_recovered=1
  if [ "$kodi_state" = inactive ]; then
    if run_recovery_step sh "$scripts/display-recovery.sh" --restore "$session.display-pending" \
         "${LUNA_VIDEO_DISABLE:-/sys/class/video/disable_video}" \
         "${LUNA_VDEC_STATUS:-/sys/class/vdec/vdec_status}" \
         "${LUNA_FB0_BLANK:-/sys/class/graphics/fb0/blank}" \
         "${LUNA_FB1_BLANK:-/sys/class/graphics/fb1/blank}" "$KODI_UNIT"; then
      rm -f "$session.display-pending"
    else
      display_recovered=0
      echo 'Luna NG watchdog: display recovery failed or timed out; keeping marker' >&2
    fi
  elif [ "$kodi_state" = unknown ]; then
    display_recovered=0
    [ -e "$session.display-pending" ] || printf '%s\n' unverified > "$session.display-pending"
    echo 'Luna NG watchdog: Kodi state unknown; refusing display writes' >&2
  elif [ -e "$session.display-pending" ]; then
    # Kodi already owns the display again. Never write the decoder/FB nodes
    # while it is active. A bounded read-only check can retire a stale marker
    # after Kodi itself has unblanked the framebuffer.
    display_recovered=0
    if run_recovery_step sh "$scripts/display-recovery.sh" --verify "$session.display-pending" \
         "${LUNA_FB0_BLANK:-/sys/class/graphics/fb0/blank}" \
         "${LUNA_FB1_BLANK:-/sys/class/graphics/fb1/blank}" &&
       [ "$(kodi_service_state)" = active ]; then
      rm -f "$session.display-pending"
      display_recovered=1
      echo 'Luna NG watchdog: Kodi reclaimed the display; cleared pending marker' >&2
    else
      echo 'Luna NG watchdog: framebuffer ownership is not yet verified; retaining marker' >&2
    fi
  fi
  # Only disarm when Kodi is actually active, not merely because a start
  # request was accepted. An immediate service crash must stay recoverable.
  kodi_recovered=0
  if [ "$kodi_initial" = inactive ]; then
    # Honor manually stopped Kodi: restore the native state, never start Kodi.
    [ "$(kodi_service_state)" = inactive ] && kodi_recovered=1
  elif [ "$(kodi_service_state)" = active ]; then
    kodi_recovered=1
  else
    attempt=0
    while [ "$attempt" -lt 15 ]; do
      kodi_state="$(kodi_service_state)"
      if [ "$kodi_state" = active ]; then
        kodi_recovered=1
        break
      fi
      now="$(date +%s)"
      remaining=$((RECOVERY_TIMEOUT - (now - recovery_started)))
      if [ "$remaining" -le 0 ]; then
        # Restoring controls used the budget. Make one bounded best-effort
        # Kodi start rather than leaving the GUI down without trying.
        [ "$attempt" -eq 0 ] || break
        per_attempt=1
      else
        per_attempt="$RECOVERY_HELPER_TIMEOUT"
        if [ "$remaining" -lt "$per_attempt" ]; then
          per_attempt="$remaining"
        fi
      fi
      if [ "$kodi_state" = inactive ]; then
        timeout -k 2 "$per_attempt" "$SYSTEMCTL" start "$KODI_UNIT" 9>&- 2>/dev/null || true
      else
        echo 'Luna NG watchdog: Kodi status unknown; waiting without start/display writes' >&2
      fi
      if [ "$(kodi_service_state)" = active ]; then
        kodi_recovered=1
        break
      fi
      attempt=$((attempt + 1))
      [ "$attempt" -ge 15 ] || sleep 2
    done
  fi
  if [ "$kodi_recovered" -ne 1 ]; then
    if [ "$kodi_initial" = inactive ]; then
      echo 'Luna NG watchdog: initially inactive Kodi changed state; retaining recovery' >&2
    else
      echo 'Luna NG watchdog: Kodi could not restart after 15 attempts; recovery remains armed' >&2
    fi
    timeout -k 2 12 sh "$scripts/hang-diagnostics.sh" "$session_unit" "$session" >&2 || true
    rm -f "$session.ready" "$stream_ready"
    flock -u 9 2>/dev/null || true
    exit 1
  fi
  if [ "$aml_recovered" -ne 1 ] || [ "$hdmi_recovered" -ne 1 ] || [ "$tsync_recovered" -ne 1 ] ||
     [ "$network_recovered" -ne 1 ] || [ "$governor_recovered" -ne 1 ] || [ "$display_recovered" -ne 1 ]; then
    echo "Luna NG watchdog: Kodi active but AML/HDMI/tsync/tuning recovery is still incomplete" >&2
    flock -u 9 2>/dev/null || true
    exit 1
  fi
  # Session-owned files must be gone before disarming the recovery marker.
  # Otherwise a cleanup failure would be ignored on the next launch.
  if ! luna_cleanup_owned_launch_files "$session"; then
    echo 'Luna NG watchdog: private file cleanup incomplete' >&2
    flock -u 9 2>/dev/null || true
    exit 1
  fi
  if ! luna_write_receipt "$session" 0; then
    echo "Luna NG watchdog: unable to disarm recovered Kodi session" >&2
    flock -u 9 2>/dev/null || true
    exit 1
  fi
fi
if [ "$armed" -ne 1 ] &&
   ! luna_cleanup_owned_launch_files "$session"; then
  echo 'Luna NG watchdog: unarmed private file cleanup incomplete' >&2
  flock -u 9 2>/dev/null || true
  exit 1
fi

rm -f "$session" "$session.ready" "$stream_ready" "$session.moonlight-pid" "$session.cleanup" "$session.startup" "$session.journals" "$session.boot-id" "$session.owner" "$session.unit" "$session.display-pending" \
      "$session.native-not-started" "$session.native-launch-attempted" "$session.native-exited" "$session.kodi-initial"
[ -n "$aml_state" ] && rm -f "$aml_state"
[ -n "$governor_state" ] && rm -f "$governor_state"
[ -n "$network_state" ] && rm -f "$network_state"
[ -n "$tsync_state" ] && rm -f "$tsync_state"
[ -n "$hdmi_state" ] && { [ -s "$hdmi_state" ] || rm -f "$hdmi_state"; }
flock -u 9 2>/dev/null || true
exit 0
