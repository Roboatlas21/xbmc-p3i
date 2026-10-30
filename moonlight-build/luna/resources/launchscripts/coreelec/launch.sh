#!/bin/sh
set -u
BIN="${MOONLIGHT_BINARY:-moonlight}"
LOG="${LUNA_LOG_DIR:-/storage/.kodi/userdata/addon_data/script.luna}/logfile"
RESUME_SETTINGS="${LUNA_RESUME_SETTINGS:-$(dirname "$LOG")/settings.xml}"
VIDEO_DISABLE="${LUNA_VIDEO_DISABLE:-/sys/class/video/disable_video}"
VDEC_STATUS="${LUNA_VDEC_STATUS:-/sys/class/vdec/vdec_status}"
SYSTEMCTL="${LUNA_SYSTEMCTL:-systemctl}"
SYSTEMD_RUN="${LUNA_SYSTEMD_RUN:-systemd-run}"
KILLALL="${LUNA_KILLALL:-killall}"
KODI_UNIT="${LUNA_KODI_UNIT:-kodi.service}"
SESSION_UNIT="${LUNA_SESSION_UNIT:-luna-ng-session-$$.service}"
CPUFREQ_ROOT="${LUNA_CPUFREQ_ROOT:-/sys/devices/system/cpu/cpufreq}"
NET_CORE_ROOT="${LUNA_NET_CORE_ROOT:-/proc/sys/net/core}"
TSYNC_ENABLE="${LUNA_TSYNC_ENABLE:-/sys/class/tsync/enable}"
HDMI_ROOT="${LUNA_HDMI_ROOT:-/sys/class/amhdmitx/amhdmitx0}"
STARTUP_TIMEOUT="${LUNA_STARTUP_TIMEOUT:-20}"
case "$STARTUP_TIMEOUT" in
  ''|*[!0-9]*) STARTUP_TIMEOUT=20 ;;
esac
if [ "$STARTUP_TIMEOUT" -lt 5 ] || [ "$STARTUP_TIMEOUT" -gt 120 ]; then
  STARTUP_TIMEOUT=20
fi
mkdir -p "$(dirname "$LOG")" || exit 1

for command in "$BIN" "$SYSTEMCTL" "$SYSTEMD_RUN" "$KILLALL"; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "Required executable not found: $command" >&2
    exit 127
  fi
done

scripts="$(CDPATH='' cd "$(dirname "$0")" && pwd)" || exit 1
. "$scripts/tsync.sh" || exit 126
. "$scripts/aml-recovery.sh" || exit 126
. "$scripts/recovery-journal.sh" || exit 126
. "$scripts/kodi-state.sh" || exit 126
. "$scripts/session-ownership.sh" || exit 126
bin_dir=''
case "$BIN" in
  */*)
    bin_dir="$(CDPATH='' cd "$(dirname "$BIN")" && pwd)" || {
      echo "Moonlight binary directory is unavailable: $BIN" >&2
      exit 127
    }
    ;;
esac
# Only a snapshot in Moonlight's own binary directory can be passed on
# to a detached session. Never unlink an arbitrary environment-supplied path.
stream_config="${LUNA_STREAM_CONFIG:-}"
cleanup_stream_config() {
  [ -n "$stream_config" ] || return 0
  if luna_owned_file_valid "$bin_dir" "$stream_config" stream; then
    rm -f "$stream_config"
  fi
}
if [ -n "$stream_config" ]; then
  if ! luna_owned_file_valid "$bin_dir" "$stream_config" stream; then
    echo 'Luna NG: rejecting unsafe session configuration path' >&2; exit 1
  fi
  if [ ! -f "$stream_config" ] || [ ! -r "$stream_config" ] ||
     [ -L "$stream_config" ]; then
    echo 'Luna NG: per-session Moonlight configuration is unavailable' >&2
    exit 1
  fi
fi
# A private handshake file lets Kodi distinguish a queued systemd job from
# an actual launcher process that obtained its launch lock.
launch_ack="${LUNA_LAUNCH_ACK:-}"
if [ -n "$launch_ack" ]; then
  if ! luna_owned_file_valid "$bin_dir" "$launch_ack" ack; then
    echo 'Luna NG: rejecting unsafe launcher acknowledgment path' >&2; exit 1
  fi
  if [ ! -f "$launch_ack" ] || [ ! -w "$launch_ack" ] ||
     [ -L "$launch_ack" ]; then
    echo 'Luna NG: launcher acknowledgment file is unavailable' >&2
    exit 1
  fi
fi
launch_ack_state=''
luna_handoff_abandoned() {
  [ -n "$launch_ack" ] &&
    { luna_receipt_is "$launch_ack.abandoned" abandoned ||
      luna_receipt_is "$launch_ack" abandoned; }
}
cleanup_launch_files() {
  if [ -n "$launch_ack" ] &&
     luna_owned_file_valid "$bin_dir" "$launch_ack" ack; then
    if [ "$launch_ack_state" = armed ] ||
       [ "$launch_ack_state" = abandoned ] ||
       luna_handoff_abandoned; then
      # Kodi may have exited before its Python finally block ran.
      # The detached session owns this receipt until native cleanup.
      if luna_owned_file_valid "$bin_dir" "$launch_ack.abandoned" abandoned; then
        rm -f "$launch_ack.abandoned" || true
      fi
      rm -f "$launch_ack" || true
    elif [ -f "$launch_ack" ]; then
      luna_write_receipt "$launch_ack" failed 2>/dev/null || true
    fi
  fi
  cleanup_stream_config
}
trap cleanup_launch_files 0

if [ ! -r "$scripts/kodi-watchdog.sh" ]; then
  echo "Missing Kodi recovery watchdog" >&2
  exit 126
fi
resume_helper="$scripts/../../lib/util/resumestate.py"
if [ ! -r "$resume_helper" ]; then
  echo "Missing Luna resume-state helper" >&2
  exit 126
fi

# The watchdog restarts when recovery is incomplete. Prevent a newer
# stream from racing old session cleanup, even after Kodi has restarted.
# This stable lock also serializes simultaneous Kodi plugin launch attempts.
exec 8>"$(dirname "$LOG")/.luna-launch.lock" || exit 1
if ! flock -n 8; then
  echo 'Luna NG: another stream launcher is active' >&2
  exit 1
fi
# This is the earliest reliable evidence that the transient unit actually
# executed this launcher. No Kodi or native resource has been taken yet.
if [ -n "$launch_ack" ] &&
   ! luna_write_receipt "$launch_ack" entered; then
  echo 'Luna NG: unable to publish launcher acknowledgment' >&2
  exit 1
fi

reconcile_pending_session() {
  previous="$1"
  [ -r "$previous.boot-id" ] || {
    echo "Luna NG: old session $previous has no recovery metadata; manual recovery required" >&2
    return 1
  }
  # The manifest is mutable recovery data. Reject unsafe paths *before*
  # attempting old-boot HDMI restoration or any journal deletion.
  if ! luna_load_session_journals "$previous"; then
    echo 'Luna NG: unsafe or incomplete previous-session recovery manifest' >&2
    return 1
  fi
  previous_governor="$LUNA_JOURNAL_GOV"
  previous_network="$LUNA_JOURNAL_NET"
  previous_tsync="$LUNA_JOURNAL_TSYNC"
  previous_hdmi="$LUNA_JOURNAL_HDMI"
  previous_aml="$LUNA_JOURNAL_AML"
  # An older snapshot used the fixed unit name. New snapshots record the
  # distinct service that owns their watchdog and recovery journal.
  previous_unit='luna-ng-session.service'
  if [ -e "$previous.unit" ]; then
    previous_unit="$(cat "$previous.unit" 2>/dev/null)" || return 1
  fi
  case "$previous_unit" in
    luna-ng-session.service|luna-ng-session-*.service) ;;
    *) echo 'Luna NG: invalid prior unit' >&2; return 1 ;;
  esac
  case "$previous_unit" in *[!A-Za-z0-9_.-]*) return 1 ;; esac
  if [ "$previous_unit" = "$SESSION_UNIT" ]; then
    echo 'Luna NG: refusing to recover the current unit as an old session' >&2
    return 1
  fi
  previous_boot="$(cat "$previous.boot-id" 2>/dev/null || true)"
  this_boot="$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)"
  [ -n "$previous_boot" ] && [ -n "$this_boot" ] || return 1
  if [ "$previous_boot" = "$this_boot" ]; then
    # The fixed systemd unit now belongs to this launcher. Check the exact
    # previous process start time, not the reused service's active state.
    if [ ! -r "$previous.owner" ]; then
      echo 'Luna NG: old session lacks owner identity; cannot safely reconcile same-boot state' >&2
      return 1
    fi
    IFS='|' read -r old_pid old_start < "$previous.owner" || return 1
    case "$old_pid:$old_start" in *[!0-9:]*|:*|*:) return 1 ;; esac
    if [ -r "/proc/$old_pid/stat" ]; then
      observed="$(sed 's/^.*) //' "/proc/$old_pid/stat" 2>/dev/null | awk '{print $20}')"
      if [ "$observed" = "$old_start" ]; then
        echo 'Luna NG: earlier launcher still alive; leaving its recovery intact' >&2
        return 1
      fi
    fi
  fi
  if [ "$previous_boot" != "$this_boot" ]; then
    # Kernel sysfs was recreated at reboot; never apply old CPU/AML/tsync
    # values to a new kernel. HDMI ALLM can persist, so restore its content
    # hint conservatively before retiring this previous-boot snapshot.
    if ! timeout -k 2 15 python3 "$scripts/../../lib/util/hdmigamemode.py" restore "$previous_hdmi"; then
      echo 'Luna NG: old-boot HDMI recovery failed; new stream blocked' >&2
      return 1
    fi
    # Previous-boot processes cannot retain native resources. Reclaim only
    # the exact private paths recorded by that old session.
    luna_cleanup_owned_launch_files "$previous" || return 1
    if ! rm -f "$previous" "$previous.ready" "$previous.stream-ready" \
          "$previous.cleanup" "$previous.startup" "$previous.moonlight-pid" "$previous.display-pending" \
          "$previous.native-not-started" "$previous.native-launch-attempted" "$previous.native-exited" "$previous.kodi-initial" \
          "$previous.journals" "$previous.launch-files" "$previous.boot-id" "$previous.owner" "$previous.unit" \
          "$previous_aml" "$previous_governor" "$previous_network" "$previous_tsync" "$previous_hdmi"; then
      return 1
    fi
    echo 'Luna NG: reconciled stale recovery from previous boot' >> "$LOG"
    return 0
  fi
  # Same boot: the old *unit*, not just its launcher PID, must have stopped.
  # A lingering native process can still own AML/audio/DRM state.
  if ! luna_session_unit_quiescent "$previous_unit" "$previous"; then
    echo 'Luna NG: prior systemd unit or native process still owns resources; recovery deferred' >&2
    return 1
  fi
  # The shared recovery flock serializes old watchdog and new launcher.
  if ! LUNA_RECOVER_ONLY=1 timeout -k 2 55 sh "$scripts/kodi-watchdog.sh" "$previous_unit" "$previous" \
      "$previous_governor" "$previous_network" "$previous_tsync" \
      "$previous_hdmi" "$previous_aml"; then
    echo 'Luna NG: previous recovery remains pending; no new stream started' >&2
    return 1
  fi
}

recovery_index=0
for old_session in "$(dirname "$LOG")"/.luna-session.????????; do
  [ -f "$old_session" ] || continue
  if luna_receipt_is "$old_session" 0; then
    if ! luna_retire_unarmed_session "$old_session" "$(dirname "$LOG")" &&
       ! luna_retire_completed_session "$old_session" "$(dirname "$LOG")"; then
      echo 'Luna NG: previous unarmed/completed session ownership not verified; new launch blocked' >&2
      exit 1
    fi
    continue
  fi
  if ! luna_receipt_is "$old_session" 1; then
    # Older launchers could die after mktemp but before writing the initial
    # receipt. Retire ONLY a truly empty, metadata-free marker under flock.
    if luna_retire_empty_session "$old_session" "$(dirname "$LOG")"; then
      continue
    fi
    echo 'Luna NG: invalid prior session arm marker; refusing a new launch' >&2
    exit 1
  fi
  if luna_handoff_abandoned; then
    launch_ack_state=abandoned
    echo 'Luna NG: Kodi withdrew new handoff; old recovery unchanged' >&2
    exit 1
  fi
  recovery_index=$((recovery_index + 1))
  if [ -n "$launch_ack" ] &&
     ! luna_write_receipt "$launch_ack" "reconciling:$recovery_index"; then
    exit 1
  fi
  if ! reconcile_pending_session "$old_session"; then
    exit 1
  fi
done

# The stable launch flock is held and all existing recovery markers were
# checked. Only now is it safe to consider aged, unreferenced lock files.
luna_gc_retired_locks "$(dirname "$LOG")"

if luna_handoff_abandoned; then
  launch_ack_state=abandoned
  echo 'Luna NG: new stream handoff was abandoned; Kodi unchanged' >&2
  exit 1
fi

# A killed mktemp/write sequence must not publish an empty .luna-session
# marker. Write the receipt under a different staging prefix and publish the
# complete 0 receipt atomically with rename; orphan staging files cannot block
# later stream launches.
session_initial="$(mktemp "$(dirname "$LOG")/.luna-initial.XXXXXXXX")" || exit 1
session="$(dirname "$LOG")/.luna-session.${session_initial##*.}"
if ! printf '0\n' > "$session_initial" ||
   ! mv -f "$session_initial" "$session"; then
  rm -f "$session_initial"
  exit 1
fi
# Positive proof that no native process has been started by this session.
# This receipt is removed immediately before a native launch is attempted.
luna_write_receipt "$session.native-not-started" not-started || exit 1
# Tie every scratch journal to this already-random, exclusive session ID.
# A SIGKILL before .journals is committed can then be reconciled safely.
luna_new_recovery_file() {
  (umask 077; set -C; : > "$1")
}
governor_state="$session.governor-state"
luna_new_recovery_file "$governor_state" || {
  rm -f "$session" "$session.native-not-started"
  exit 1
}
network_state="$session.network-state"
luna_new_recovery_file "$network_state" || {
  rm -f "$session" "$session.native-not-started" "$governor_state"
  exit 1
}
tsync_state="$session.tsync-state"
luna_new_recovery_file "$tsync_state" || {
  rm -f "$session" "$session.native-not-started" "$governor_state" "$network_state"
  exit 1
}
hdmi_state="$session.hdmi-state"
luna_new_recovery_file "$hdmi_state" || {
  rm -f "$session" "$session.native-not-started" "$governor_state" "$network_state" "$tsync_state"
  exit 1
}
aml_state="$session.aml-state"
if ! aml_snapshot "$aml_state"; then
  rm -f "$session" "$session.native-not-started" "$governor_state" "$network_state" "$tsync_state" "$hdmi_state" "$aml_state"
  exit 1
fi
stream_ready="$session.stream-ready"
if ! : > "$stream_ready" || ! {
  luna_new_recovery_file "$session.journals.pending" &&
  printf '%s\n' "$governor_state" "$network_state" "$tsync_state" "$hdmi_state" "$aml_state" > "$session.journals.pending" &&
  mv -f "$session.journals.pending" "$session.journals" &&
  luna_new_recovery_file "$session.launch-files.pending" &&
  printf '%s\n' "$bin_dir" "$stream_config" "$launch_ack" > "$session.launch-files.pending" &&
  mv -f "$session.launch-files.pending" "$session.launch-files" &&
  cat /proc/sys/kernel/random/boot_id > "$session.boot-id" &&
  { owner_start="$(sed 's/^.*) //' "/proc/$$/stat" 2>/dev/null | awk '{print $20}')" &&
    [ -n "$owner_start" ] &&
    luna_write_receipt "$session.owner" "$|$owner_start" &&
    luna_write_receipt "$session.unit" "$SESSION_UNIT"; }
}; then
  rm -f "$session" "$session.native-not-started" "$governor_state" "$network_state" "$tsync_state" "$hdmi_state" "$aml_state" \
        "$session.journals" "$session.journals.pending" "$session.launch-files" "$session.launch-files.pending" "$session.boot-id" "$session.owner" "$session.unit" "$stream_ready"
  exit 1
fi
watchdog_unit="luna-ng-watchdog-$$"
child=''
kodi_stopped=0
kodi_handoff_attempted=0
# Until the Kodi state is confirmed, this invocation has touched no runtime
# controls and must not leave an unarmed session behind on early abort.
pre_handoff_abort=1

enable_cpu_performance_governor() {
  [ "${MOONLIGHT_CPU_PERFORMANCE:-0}" = 1 ] || return 0
  [ -d "$CPUFREQ_ROOT" ] || {
    echo "Luna NG: CPU frequency policies unavailable; performance governor skipped" >> "$LOG"
    return 0
  }
  changed=0
  for governor in "$CPUFREQ_ROOT"/policy*/scaling_governor; do
    [ -e "$governor" ] || continue
    if [ ! -r "$governor" ] || [ ! -w "$governor" ]; then
      echo "Luna NG: CPU governor control unavailable: $governor" >> "$LOG"
      continue
    fi
    original="$(cat "$governor" 2>/dev/null)" || continue
    case "$original" in
      ''|*[!A-Za-z0-9_-]*)
        echo "Luna NG: unexpected CPU governor value at $governor" >> "$LOG"
        continue ;;
    esac
    [ "$original" = performance ] && continue
    # Record before the kernel write; partial sysfs success is still reversible.
    if ! printf '%s|%s\n' "$governor" "$original" >> "$governor_state"; then
      echo "Luna NG: cannot journal CPU governor; aborting handoff" >> "$LOG"
      return 1
    fi
    if ! printf '%s' performance > "$governor" 2>/dev/null; then
      echo "Luna NG: failed to set performance governor: $governor" >> "$LOG"
      return 1
    fi
    current="$(cat "$governor" 2>/dev/null)" || current=''
    if [ "$current" != performance ]; then
      echo "Luna NG: performance governor was not confirmed: $governor" >> "$LOG"
      return 1
    fi
    changed=$((changed + 1))
  done
  if [ "$changed" -gt 0 ]; then
    echo "Luna NG: performance CPU governor enabled for $changed policy/policies" >> "$LOG"
  fi
}

restore_cpu_governors() {
  timeout -k 2 15 sh -c '. "$1"; restore_tuning_journal "$2" governor' \
    luna-governor "$scripts/recovery-journal.sh" "$governor_state" 9>&- >> "$LOG" 2>&1
}

tune_network_value() {
  path="$1"
  target="$2"
  [ -e "$path" ] || {
    echo "Luna NG: network tuning path unavailable: $path" >> "$LOG"
    return 0
  }
  if [ ! -r "$path" ] || [ ! -w "$path" ]; then
    echo "Luna NG: network tuning control unavailable: $path" >> "$LOG"
    return 0
  fi
  original="$(cat "$path" 2>/dev/null)" || return 0
  case "$original" in
    ''|*[!0-9]*)
      echo "Luna NG: unexpected network tuning value at $path" >> "$LOG"
      return 0 ;;
  esac
  # Do not reduce a larger existing socket buffer or kernel setting.
  if [ "$original" -ge "$target" ]; then
    return 0
  fi
  if ! printf '%s|%s|%s\n' "$path" "$original" "$target" >> "$network_state"; then
    echo "Luna NG: cannot journal network tuning; aborting handoff" >> "$LOG"
    return 1
  fi
  if ! printf '%s' "$target" > "$path" 2>/dev/null; then
    echo "Luna NG: failed to tune network value: $path" >> "$LOG"
    return 1
  fi
  current="$(cat "$path" 2>/dev/null)" || current=''
  if [ "$current" != "$target" ]; then
    echo "Luna NG: network tuning was not confirmed: $path" >> "$LOG"
    return 1
  fi
}

enable_network_tuning() {
  tune_network_value "$NET_CORE_ROOT/rmem_max" 33554432 || return 1
  tune_network_value "$NET_CORE_ROOT/rmem_default" 2097152 || return 1
  tune_network_value "$NET_CORE_ROOT/netdev_max_backlog" 5000 || return 1

  if [ -s "$network_state" ]; then
    echo "Luna NG: Skupi-style socket-buffer tuning enabled" >> "$LOG"
  fi
}

restore_network_tuning() {
  timeout -k 2 15 sh -c '. "$1"; restore_tuning_journal "$2" network' \
    luna-network "$scripts/recovery-journal.sh" "$network_state" 9>&- >> "$LOG" 2>&1
}

disable_tsync() {
  [ -e "$TSYNC_ENABLE" ] || {
    echo "Luna NG: Amlogic tsync control unavailable; skipped" >> "$LOG"
    return 0
  }
  if [ ! -r "$TSYNC_ENABLE" ] || [ ! -w "$TSYNC_ENABLE" ]; then
    echo "Luna NG: Amlogic tsync control is not readable/writable; skipped" >> "$LOG"
    return 0
  fi
  original="$(read_tsync "$TSYNC_ENABLE")" || {
    echo "Luna NG: unreadable or unexpected Amlogic tsync value; skipped" >> "$LOG"
    return 0
  }
  [ "$original" = 0 ] && {
    echo "Luna NG: Amlogic tsync already disabled" >> "$LOG"
    return 0
  }
  # Never modify tsync if its recovery record could not be persisted.
  if ! printf '%s|%s|0\n' "$TSYNC_ENABLE" "$original" > "$tsync_state"; then
    echo "Luna NG: cannot journal tsync; aborting handoff" >> "$LOG"
    return 1
  fi
  if ! printf '%s' 0 > "$TSYNC_ENABLE" 2>/dev/null; then
    echo "Luna NG: failed to disable Amlogic tsync" >> "$LOG"
    return 1
  fi
  current="$(read_tsync "$TSYNC_ENABLE")" || current=''
  if [ "$current" != 0 ]; then
    echo "Luna NG: Amlogic tsync disable was not confirmed; restoring" >> "$LOG"
    restore_tsync || true
    return 1
  fi
  echo "Luna NG: Amlogic tsync disabled for Moonlight" >> "$LOG"
}

restore_tsync() {
  timeout -k 2 15 sh -c '. "$1"; restore_tsync_snapshot "$2"' \
    luna-tsync "$scripts/tsync.sh" "$tsync_state" 9>&- >> "$LOG" 2>&1
}

restore_kodi_display() {
  timeout -k 2 15 sh "$scripts/display-recovery.sh" --restore "$session.display-pending" "$VIDEO_DISABLE" "$VDEC_STATUS" \
    "${LUNA_FB0_BLANK:-/sys/class/graphics/fb0/blank}" \
    "${LUNA_FB1_BLANK:-/sys/class/graphics/fb1/blank}" "$KODI_UNIT" 9>&- >> "$LOG" 2>&1
}

child_running() {
  [ -n "$child" ] || return 1
  kill -0 "$child" 2>/dev/null || return 1
  # A restricted/mismatched proc mount must not bypass the startup timeout.
  # The shell still knows whether its own child exists via kill -0.
  [ -r "/proc/$child/stat" ] || return 0
  line="$(cat "/proc/$child/stat" 2>/dev/null)" || return 0
  fields="${line##*) }"
  state="${fields%% *}"
  case "$state" in Z|X|x) return 1 ;; esac
  kill -0 "$child" 2>/dev/null
}

terminate_orphaned_moonlight() {
  # Only terminate Moonlight processes in this Luna session's exact cgroup.
  # Never scan/kill every instance of the same executable globally.
  group="$(timeout -k 2 5 "$SYSTEMCTL" show --property=ControlGroup --value "$SESSION_UNIT" 8>&- 9>&- 2>/dev/null || true)"
  case "$group" in /*) ;; *) return 0 ;; esac
  expected="$(readlink -f "$BIN" 2>/dev/null || printf '%s' "$BIN")"
  candidates=''
  for proc in /proc/[0-9]*/cgroup; do
    [ -r "$proc" ] || continue
    pid="${proc#/proc/}"; pid="${pid%/cgroup}"
    [ "$pid" = "$$" ] && continue
    awk -F: -v g="$group" '$3 == g {found=1} END {exit !found}' "$proc" 2>/dev/null || continue
    exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
    luna_native_executable_matches "$exe" "$expected" || continue
    started="$(sed 's/^.*) //' "/proc/$pid/stat" 2>/dev/null | awk '{print $20}')"
    [ -n "$started" ] || continue
    candidates="${candidates}${pid}|${started} "
    kill -TERM "$pid" 2>/dev/null || true
  done
  [ -z "$candidates" ] && return 0
  sleep 1
  survivors=0
  for item in $candidates; do
    pid="${item%%|*}"; started="${item#*|}"
    now="$(sed 's/^.*) //' "/proc/$pid/stat" 2>/dev/null | awk '{print $20}')"
    [ "$now" = "$started" ] || continue
    awk -F: -v g="$group" '$3 == g {found=1} END {exit !found}' "/proc/$pid/cgroup" 2>/dev/null || continue
    exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
    if luna_native_executable_matches "$exe" "$expected"; then
      kill -KILL "$pid" 2>/dev/null || true
      # A process lingering after KILL cannot be treated as proof that
      # all decoder/DRM handles have been relinquished.
      sleep 0.1
      still="$(sed 's/^.*) //' "/proc/$pid/stat" 2>/dev/null | awk '{print $1 " " $20}')"
      case "$still" in
        "Z $started"|"X $started"|'' ) : ;;
        *) survivors=1 ;;
      esac
    fi
  done
  [ "$survivors" -eq 0 ]
}

startup_ready() {
  [ -n "$child" ] || return 1

  # Moonlight writes this marker only after LiStartConnection succeeds.
  # Evdev descriptors and decoder activity occur too early to prove that the
  # stream connection finished starting and must not bypass the timeout.
  [ -r "$stream_ready" ] && grep -Fxq ready "$stream_ready" 2>/dev/null
}

cleanup() {
  rc=$?
  trap - 0
  # This marker is written only on exit; a long-running stream has no timeout.
  # An independent watchdog uses its epoch to detect a stuck cleanup process.
  printf '%s\n' "$(date +%s)" > "$session.cleanup" || true
  # Kernel-managed recovery ownership, held before touching decoder/sysfs/Kodi.
  recovery_lock="$session.recovery.lock"
  exec 9>"$recovery_lock" || exit "$rc"
  echo "Luna NG: waiting for exclusive recovery ownership" >> "$LOG"
  flock 9 || { echo "Luna NG: unable to acquire recovery lock" >> "$LOG"; exit "$rc"; }
  # Keep recovery ownership on the launcher, not its short-lived helpers.
  # External commands below inherit fd 9 unless explicitly closed; the lock
  # remains held by this shell until cleanup completes.
  echo "LUNA_STARTUP_RESULT phase=cleanup exit_status=$rc ready=$(startup_ready && printf 1 || printf 0) child=${child:-none}" >> "$LOG"

  if [ -n "$child" ] && child_running; then
    kill -TERM "$child" 2>/dev/null || true
    grace=0
    while child_running && [ "$grace" -lt 3 ]; do
      sleep 1
      grace=$((grace + 1))
    done
    child_running && kill -KILL "$child" 2>/dev/null || true
  fi
  if [ -n "$child" ]; then
    wait "$child" 2>/dev/null || true
  fi

  # Do not return display ownership to Kodi while an exact copy of Luna's
  # Moonlight binary is still alive. This catches descendants that escaped
  # the shell's direct child PID bookkeeping.
  # A surviving native process must keep this session's recovery identity.
  native_clean=1
  terminate_orphaned_moonlight || native_clean=0
  if [ "$native_clean" -eq 1 ] && [ -n "$child" ] && ! child_running; then
    luna_write_receipt "$session.native-exited" exited || native_clean=0
  fi
  if [ "$native_clean" -eq 1 ] &&
     ! luna_native_processes_stopped "$session" "$SESSION_UNIT"; then
    native_clean=0
  fi
  if [ "$native_clean" -ne 1 ]; then
    # Never reset decoder/HDMI/FB state while native resource ownership is
    # uncertain. Leave the armed marker and journals for independent recovery.
    echo 'Luna NG: native process termination unverified; refusing display recovery' >> "$LOG"
    flock -u 9 2>/dev/null || true
    rm -f "$session.cleanup"
    exit "$rc"
  fi

  hdmi_recovered=1
  if ! timeout -k 2 15 python3 "$scripts/../../lib/util/hdmigamemode.py" restore "$hdmi_state" 9>&- >> "$LOG" 2>&1; then
    hdmi_recovered=0
    echo "Luna NG: HDMI restoration incomplete; watchdog remains armed" >> "$LOG"
  fi
  tsync_recovered=1
  if ! restore_tsync; then
    tsync_recovered=0
    echo "Luna NG: tsync restoration incomplete; watchdog remains armed" >> "$LOG"
  fi
  aml_recovered=1
  if ! timeout -k 2 15 sh -c '. "$1"; aml_restore "$2"' luna-aml \
        "$scripts/aml-recovery.sh" "$aml_state" 9>&- >> "$LOG" 2>&1; then
    aml_recovered=0
    echo "Luna NG: AML restoration incomplete; watchdog remains armed for retry" >> "$LOG"
  fi
  network_recovered=1
  restore_network_tuning || network_recovered=0
  governor_recovered=1
  restore_cpu_governors || governor_recovered=0

  # Confirm Kodi's state AFTER the stop attempt and AFTER terminating Moonlight.
  # A failed systemctl stop may leave Kodi active, so never trust a flag set
  # before the stop operation as permission to touch video/FB sysfs.
  kodi_state="$(kodi_service_state)"
  if [ "$kodi_state" = inactive ]; then
    kodi_stopped=1
  else
    kodi_stopped=0
  fi

  # The prior state was captured before multiple restoration helpers.
  # Never infer current display ownership from that stale observation.
  kodi_state="$(kodi_service_state)"
  display_recovered=1
  if [ "$kodi_state" = inactive ] &&
     { [ "$kodi_handoff_attempted" -eq 1 ] || [ -n "$child" ]; }; then
    if restore_kodi_display; then
      rm -f "$session.display-pending"
    else
      display_recovered=0
      echo 'Luna NG: display recovery failed or timed out; retaining marker' >> "$LOG"
    fi
  elif [ "$kodi_state" = unknown ]; then
    display_recovered=0
    echo 'Luna NG: Kodi state unknown; refusing display writes' >> "$LOG"
  fi

  if [ "$kodi_handoff_attempted" -eq 1 ]; then
    if { [ "$kodi_state" = active ] ||
         { [ "$kodi_state" = inactive ] &&
           timeout -k 2 15 "$SYSTEMCTL" start "$KODI_UNIT" 8>&- 9>&- &&
           [ "$(kodi_service_state)" = active ]; }; }; then
      if [ "$aml_recovered" -eq 1 ] && [ "$hdmi_recovered" -eq 1 ] && [ "$tsync_recovered" -eq 1 ] &&
         [ "$network_recovered" -eq 1 ] && [ "$governor_recovered" -eq 1 ] && [ "$display_recovered" -eq 1 ] &&
         [ "$native_clean" -eq 1 ]; then
        if luna_write_receipt "$session" 0; then
          rm -f "$session" "$session.ready"
        else
          echo "Luna NG: unable to disarm recovery marker" >> "$LOG"
        fi
      else
        echo "Luna NG: Kodi active, AML/tsync/HDMI/CPU/network restoration still pending" >> "$LOG"
      fi
    else
      echo "Luna NG: Kodi restart failed; recovery watchdog remains armed" >&2
    fi
  else
    if [ "${initial_kodi_state:-}" = inactive ]; then
      # Kodi was already stopped by the user. Never start it merely because
      # Moonlight ran; nevertheless require every restoration to succeed
      # before retiring this now-armed native/kernel recovery snapshot.
      if [ "$kodi_state" = inactive ] &&
         [ "$aml_recovered" -eq 1 ] && [ "$hdmi_recovered" -eq 1 ] &&
         [ "$tsync_recovered" -eq 1 ] && [ "$network_recovered" -eq 1 ] &&
         [ "$governor_recovered" -eq 1 ] && [ "$display_recovered" -eq 1 ] &&
         [ "$native_clean" -eq 1 ]; then
        if luna_write_receipt "$session" 0; then
          rm -f "$session" "$session.ready"
        fi
      else
        echo 'Luna NG: initially stopped Kodi; native recovery incomplete, keeping session armed' >> "$LOG"
      fi
    elif [ "$kodi_state" = unknown ]; then
      echo 'Luna NG: cannot confirm Kodi ownership; recovery remains armed' >> "$LOG"
    else
      rm -f "$session" "$session.ready"
    fi
  fi
  # A failed watchdog startup or unknown initial Kodi state occurred before
  # we touched Kodi or Moonlight. The marker is still '0', so the next launch
  # would skip it forever. Retire only this provably pre-handoff snapshot.
  if [ "$pre_handoff_abort" -eq 1 ] && [ "$kodi_handoff_attempted" -eq 0 ] &&
     [ -z "$child" ] && [ "$(cat "$session" 2>/dev/null)" = 0 ]; then
    rm -f "$session" "$session.ready"
    echo 'Luna NG: retired aborted pre-handoff session' >> "$LOG"
  fi
  # The native process and resource owners have been verified gone.
  # Until that point, never delete the configuration it might still read.
  cleanup_launch_files
  # Keep the lock inode stable for any watchdog already waiting on it.
  flock -u 9 2>/dev/null || true
  rm -f "$session.cleanup"
  rm -f "$stream_ready" "$session.startup"
  [ -s "$governor_state" ] || rm -f "$governor_state"
  [ -s "$network_state" ] || rm -f "$network_state"
  [ -s "$tsync_state" ] || rm -f "$tsync_state"
  [ -s "$hdmi_state" ] || rm -f "$hdmi_state"
  # Recovery metadata survives only when the session is still armed.
  if [ ! -e "$session" ]; then
    rm -f "$session.journals" "$session.launch-files" "$session.boot-id" "$session.owner" "$session.unit" \
          "$session.display-pending" "$session.moonlight-pid" \
          "$session.native-not-started" "$session.native-launch-attempted" "$session.native-exited" "$session.kodi-initial"
  fi

  exit "$rc"
}
trap cleanup 0
trap 'exit 129' 1
trap 'exit 130' 2
trap 'exit 143' 15

if [ -f "$LOG" ]; then mv -f "$LOG" "$LOG.old" 2>/dev/null || true; fi

# The watchdog is a separate transient service, so it survives even if this
# session process is killed. It restarts Kodi only when the armed marker says
# Luna stopped Kodi and this session unit disappears unexpectedly.
if ! "$SYSTEMD_RUN" --unit="$watchdog_unit" --collect --quiet --no-block \
    --property=Restart=on-failure --property=RestartSec=30s \
    /usr/bin/env \
      "LUNA_SYSTEMCTL=$SYSTEMCTL" \
      "LUNA_NATIVE_BIN_DIR=$bin_dir" \
      "LUNA_CPUFREQ_ROOT=$CPUFREQ_ROOT" \
      "LUNA_NET_CORE_ROOT=$NET_CORE_ROOT" \
      "LUNA_TSYNC_ENABLE=$TSYNC_ENABLE" \
      "LUNA_AML_LOW_LATENCY=${LUNA_AML_LOW_LATENCY:-/sys/module/aml_media/parameters/use_low_latency}" \
      "LUNA_AML_SKIP_POLICY=${LUNA_AML_SKIP_POLICY:-/sys/module/amvdec_h265/parameters/nal_skip_policy}" \
      "LUNA_AML_REF_FLAGS=${LUNA_AML_REF_FLAGS:-/sys/module/amvdec_h265/parameters/ref_frame_mark_flag}" \
      "LUNA_HDMI_ROOT=$HDMI_ROOT" \
      "LUNA_STARTUP_TIMEOUT=$STARTUP_TIMEOUT" \
      "LUNA_KODI_UNIT=$KODI_UNIT" \
      "LUNA_FB0_BLANK=${LUNA_FB0_BLANK:-/sys/class/graphics/fb0/blank}" \
      "LUNA_FB1_BLANK=${LUNA_FB1_BLANK:-/sys/class/graphics/fb1/blank}" \
      "LUNA_VIDEO_DISABLE=$VIDEO_DISABLE" \
      "LUNA_VDEC_STATUS=$VDEC_STATUS" \
      "LUNA_AML_LOW_LATENCY=${LUNA_AML_LOW_LATENCY:-/sys/module/aml_media/parameters/use_low_latency}" \
      "LUNA_AML_SKIP_POLICY=${LUNA_AML_SKIP_POLICY:-/sys/module/amvdec_h265/parameters/nal_skip_policy}" \
      "LUNA_AML_REF_FLAGS=${LUNA_AML_REF_FLAGS:-/sys/module/amvdec_h265/parameters/ref_frame_mark_flag}" \
      /bin/sh "$scripts/kodi-watchdog.sh" "$SESSION_UNIT" "$session" "$governor_state" "$network_state" "$tsync_state" "$hdmi_state" "$aml_state"; then
  echo "Unable to start Luna NG Kodi recovery watchdog" >&2
  exit 1
fi

ready=0
tries=0
while [ "$tries" -lt 30 ]; do
  if [ -r "$session.ready" ]; then
    ready=1
    break
  fi
  tries=$((tries + 1))
  sleep 0.1
done
if [ "$ready" -ne 1 ]; then
  echo "Kodi recovery watchdog did not initialize; refusing to stop Kodi" >&2
  exit 1
fi

initial_kodi_state="$(kodi_service_state)"
if [ "$initial_kodi_state" = unknown ]; then
  echo 'Luna NG: Kodi service state unknown; refusing display handoff' >&2
  exit 1
fi
# Arm recovery BEFORE any potential native, kernel, decoder or HDMI changes,
# whether Kodi started active or was already manually stopped. Record the
# initial state so the watchdog does not start a previously stopped Kodi.
luna_write_receipt "$session.kodi-initial" "$initial_kodi_state" || exit 1
luna_write_receipt "$session" 1 || exit 1
pre_handoff_abort=0
printf '%s\n' "$(date +%s)" > "$session.startup" || exit 1
# This second phase proves the old recovery was reconciled, watchdog is
# alive and the native recovery marker is armed. Only now may Kodi treat
# the handoff request as accepted.
if [ -n "$launch_ack" ]; then
  if luna_handoff_abandoned; then
    launch_ack_state=abandoned
    echo 'Luna NG: handoff abandoned before arming; preserving Kodi' >&2
    exit 1
  fi
  luna_write_receipt "$launch_ack" armed || exit 1
  launch_ack_state=armed
  # Stop Kodi only after it has actually observed the handoff receipt.
  # Standalone launcher tests without a Kodi caller retain legacy behavior.
  if [ "${LUNA_ACK_REQUIRED:-0}" = 1 ]; then
    ack_seen=0
    ack_attempt=0
    while [ "$ack_attempt" -lt 30 ]; do
      if luna_receipt_is "$launch_ack" observed; then
        ack_seen=1
        break
      fi
      ack_attempt=$((ack_attempt + 1))
      sleep 0.1
    done
    if [ "$ack_seen" -ne 1 ]; then
      launch_ack_state=failed
      luna_write_receipt "$launch_ack" failed || true
      echo 'Luna NG: Kodi did not acknowledge handoff; Kodi remains running' >&2
      exit 1
    fi
  fi
fi
if [ "$initial_kodi_state" = active ]; then
  kodi_handoff_attempted=1

  # Do not let Kodi perform its normal application shutdown here. A graceful
  # kodi.bin exit can execute the user's CEC shutdown action and put the TV
  # into standby just because Luna is starting. Kill only kodi.bin first so
  # evdev/audio/CEC resources are released without Kodi's shutdown callbacks,
  # then stop the wrapper service immediately to cancel Restart=always.
  "$KILLALL" -KILL kodi.bin 2>/dev/null || true

  if ! timeout -k 2 12 "$SYSTEMCTL" stop "$KODI_UNIT" 8>&- 9>&-; then
    echo "Luna NG: unable to complete stop of $KODI_UNIT; cleanup will recheck state" >&2
    exit 1
  fi
  if [ "$(kodi_service_state)" != inactive ]; then
    echo 'Luna NG: Kodi stop not confirmed; refusing display takeover' >&2
    exit 1
  fi
  kodi_stopped=1

  # Kodi owned the GUI/OSD KMS plane before this handoff. CoreELEC 21 NG
  # cannot use Vortex's CE22 pidfd_getfd DRM-fd borrowing path, so authorize
  # Moonlight's OSD renderer to reuse that attached ARGB overlay only while
  # Kodi is explicitly stopped by this launcher.
  export MOONLIGHT_OSD_TAKEOVER=1
else
  unset MOONLIGHT_OSD_TAKEOVER
fi

if [ "${MOONLIGHT_VIDEO_PIPELINE:-aml}" = aml-vfm ]; then
  if [ -z "${MOONLIGHT_VFM_ROOT:-}" ]; then
    echo "AML-VFM: missing experimental runtime directory" >> "$LOG"
    exit 1
  fi
  script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
  if ! timeout -k 2 "$STARTUP_TIMEOUT" python3 "$script_dir/../../lib/util/vfmruntime.py" load "$MOONLIGHT_VFM_ROOT" >> "$LOG" 2>&1; then
    echo "AML-VFM: prototype driver setup failed; restoring Kodi" >> "$LOG"
    exit 1
  fi
fi

disable_tsync || exit 1
enable_network_tuning || exit 1
enable_cpu_performance_governor || exit 1

if [ "${MOONLIGHT_TV_GAME_MODE:-0}" = 1 ]; then
  timeout -k 2 5 python3 "$scripts/../../lib/util/hdmigamemode.py" enable "$hdmi_state" "$HDMI_ROOT" >> "$LOG" 2>&1 || true
fi

if [ -n "$bin_dir" ]; then
  cd "$bin_dir" || {
    echo "Unable to enter Moonlight binary directory: $bin_dir" >&2
    exit 1
  }
fi

# Descriptor 8 belongs to this launcher. A Moonlight child must not retain
# the launch lock if the parent is killed unexpectedly.
# When native launch begins, a crash without PID or exit receipt must not
# be misread as a cleanly retired session.
rm -f "$session.native-not-started" || exit 1
luna_write_receipt "$session.native-launch-attempted" attempted || exit 1
if [ -n "$stream_config" ]; then
  MOONLIGHT_READY_FILE="$stream_ready" "$BIN" stream -config "$stream_config" -app "$@" 8>&- 9>&- >> "$LOG" 2>&1 &
else
  MOONLIGHT_READY_FILE="$stream_ready" "$BIN" stream -app "$@" 8>&- 9>&- >> "$LOG" 2>&1 &
fi
child=$!
# Record identity after exec, not the transient shell interpreter.
# A bounded retry also handles a slow exec without delaying stream startup.
expected_bin="$(readlink -f "$BIN" 2>/dev/null || printf '%s' "$BIN")"
identity_attempt=0
while [ "$identity_attempt" -lt 20 ] && child_running; do
  exe="$(readlink "/proc/$child/exe" 2>/dev/null || true)"
  if [ "$exe" = "$expected_bin" ]; then
    start_time="$(sed 's/^.*) //' "/proc/$child/stat" 2>/dev/null | awk '{print $20}')"
    if [ -n "$start_time" ]; then
      printf '%s|%s|%s\n' "$child" "$start_time" "$exe" > "$session.moonlight-pid"
      break
    fi
  fi
  identity_attempt=$((identity_attempt + 1))
  sleep 0.05
done
if [ ! -s "$session.moonlight-pid" ]; then
  echo 'Luna NG: Moonlight PID identity unavailable; watchdog will use exact cgroup membership' >> "$LOG"
fi

stream_confirmed=0
elapsed=0
while child_running && [ "$elapsed" -lt "$STARTUP_TIMEOUT" ]; do
  if startup_ready; then
    stream_confirmed=1
    break
  fi
  sleep 1
  elapsed=$((elapsed + 1))
done

# The process may exit immediately after publishing readiness. Preserve that
# successful launch confirmation even if child_running already became false.
if [ "$stream_confirmed" -eq 0 ] && startup_ready; then
  stream_confirmed=1
fi

if child_running && [ "$stream_confirmed" -ne 1 ]; then
  echo "LUNA_STARTUP_RESULT phase=timeout elapsed_s=$elapsed ready=0 exit_status=124" >> "$LOG"
  echo "Luna NG: Moonlight startup timed out after ${STARTUP_TIMEOUT}s; restoring Kodi" >> "$LOG"
  kill -TERM "$child" 2>/dev/null || true
  grace=0
  while child_running && [ "$grace" -lt 3 ]; do
    sleep 1
    grace=$((grace + 1))
  done
  child_running && kill -KILL "$child" 2>/dev/null || true
  wait "$child" 2>/dev/null || true
  exit 124
fi

if [ "$stream_confirmed" -eq 1 ]; then
  rm -f "$session.startup"
  if ! python3 "$resume_helper" set "$RESUME_SETTINGS" "$1" "${LUNA_RESUME_HOST:-}" >> "$LOG" 2>&1; then
    echo "Luna NG: failed to persist confirmed Resume state" >> "$LOG"
  fi
fi

wait "$child"
child_status=$?
echo "LUNA_STARTUP_RESULT phase=exit elapsed_s=$elapsed ready=$stream_confirmed exit_status=$child_status" >> "$LOG"
if [ "$child_status" -ne 0 ] && ! startup_ready; then
  echo "Luna NG: Moonlight exited with status ${child_status} before stream readiness; restoring Kodi" >> "$LOG"
fi
exit "$child_status"
