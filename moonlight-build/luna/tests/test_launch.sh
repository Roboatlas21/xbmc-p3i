#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
dump_failure_and_cleanup() {
  local result=$?
  trap - EXIT
  if [[ "$result" -ne 0 ]]; then
    printf '\nLUNA_TEST_FAILURE exit=%s\n' "$result" >&2
    for file in "$tmp/out" "$tmp/logs/logfile" "$tmp/logs/logfile.old" "$tmp/calls"; do
      if [[ -f "$file" ]]; then
        printf '\n--- %s ---\n' "${file#"$tmp"/}" >&2
        tail -n 120 "$file" >&2 || true
      fi
    done
  fi
  rm -rf "$tmp"
  exit "$result"
}
trap dump_failure_and_cleanup EXIT
trap 'printf "LUNA_ASSERTION_FAILURE line=%s command=%q\n" "$LINENO" "$BASH_COMMAND" >&2' ERR

mkdir -p "$tmp/coreelec-ng"
cat >"$tmp/coreelec-ng/moonlight" <<'EOF'
#!/bin/sh
printf 'cwd %s\n' "$(pwd)" >> "$CALLS"
printf 'moonlight %s\n' "$*" >> "$CALLS"
if [ -e "/proc/$$/fd/8" ]; then
  echo 'moonlight_inherited_launch_lock' >> "$CALLS"
fi
printf 'osd_takeover %s\n' "${MOONLIGHT_OSD_TAKEOVER:-unset}" >> "$CALLS"
if [ -n "${LUNA_TSYNC_ENABLE:-}" ]; then
  printf 'tsync %s\n' "$(cat "$LUNA_TSYNC_ENABLE")" >> "$CALLS"
fi
if [ -n "${LUNA_NET_CORE_ROOT:-}" ]; then
  printf 'rmem_max %s\n' "$(cat "$LUNA_NET_CORE_ROOT/rmem_max")" >> "$CALLS"
  printf 'rmem_default %s\n' "$(cat "$LUNA_NET_CORE_ROOT/rmem_default")" >> "$CALLS"
  printf 'netdev_max_backlog %s\n' "$(cat "$LUNA_NET_CORE_ROOT/netdev_max_backlog")" >> "$CALLS"
fi
if [ "${MOONLIGHT_TEST_CONNECTION_FAIL:-0}" = 1 ]; then
  exit 23
fi
if [ "${MOONLIGHT_TEST_HANG:-0}" = 1 ]; then
  trap 'exit 0' TERM
  while :; do sleep 1; done
fi
if [ -n "${MOONLIGHT_READY_FILE:-}" ]; then
  printf 'ready\n' > "$MOONLIGHT_READY_FILE"
  printf 'ready_file %s\n' "$MOONLIGHT_READY_FILE" >> "$CALLS"
fi
exit 42
EOF

cat >"$tmp/killall" <<'EOF'
#!/bin/sh
printf 'killall %s\n' "$*" >> "$CALLS"
EOF

cat >"$tmp/systemctl" <<'EOF'
#!/bin/sh
case "$1" in
  is-active)
    unit="$3"
    if [ "$unit" = "kodi.service" ]; then
      [ "$(cat "$KODI_STATE")" = active ]
    else
      [ "$(cat "$SESSION_STATE")" = active ]
    fi
    ;;
  show)
    case "$*" in
      *ActiveState*) case "$*" in *kodi.service*) cat "$KODI_STATE" ;; *) cat "$SESSION_STATE" ;; esac ;;
      *MainPID*)
        if [ -r "${LUNA_TEST_SESSION_OWNER_FILE:-/dev/null}" ]; then
          cut -d '|' -f 1 "$LUNA_TEST_SESSION_OWNER_FILE"
        else
          printf '0\n'
        fi ;;
      *ControlGroup*) : ;;
      *) exit 2 ;;
    esac
    ;;
  stop)
    shift
    printf 'systemctl stop %s\n' "$*" >> "$CALLS"
    [ "${1:-}" = "--no-block" ] && shift
    if [ "${1:-}" = "kodi.service" ]; then
      if [ "${LUNA_TEST_KODI_STOP_FAIL:-0}" = 1 ]; then exit 1; fi
      printf 'inactive\n' > "$KODI_STATE"
    else
      printf 'inactive\n' > "$SESSION_STATE"
    fi
    ;;
  start)
    printf 'systemctl start %s\n' "$2" >> "$CALLS"
    if [ "${LUNA_TEST_KODI_RESTART_FAIL:-0}" = 1 ]; then exit 1; fi
    if [ "${LUNA_TEST_KODI_BLOCK_START:-0}" = 1 ]; then /bin/sleep 20; exit 1; fi
    if [ "${LUNA_TEST_KODI_START_INACTIVE:-0}" = 1 ]; then exit 0; fi
    printf 'active\n' > "$KODI_STATE"
    if [ -n "${LUNA_TEST_KODI_RESTART_SETTINGS:-}" ]; then
      python3 - "$LUNA_TEST_KODI_RESTART_SETTINGS" <<'PY'
import sys
from xml.etree import ElementTree
root = ElementTree.parse(sys.argv[1]).getroot()
value = root.findtext('setting[@id="last_run"]')
if value != 'Desktop':
    raise SystemExit('Kodi restarted without confirmed Resume state: %r' % value)
PY
    fi
    ;;
  *)
    exit 2
    ;;
esac
EOF

cat >"$tmp/systemd-run" <<'EOF'
#!/bin/sh
printf 'systemd-run %s\n' "$*" >> "$CALLS"
session=''
for arg in "$@"; do
  case "$arg" in
    */.luna-session.*) session="$arg"; break ;;
  esac
done
[ -n "$session" ] || exit 2
: > "$session.ready"
EOF

chmod +x "$tmp/coreelec-ng/moonlight" "$tmp/killall" "$tmp/systemctl" "$tmp/systemd-run"
# Model sysfs: writes remain numeric, while each subsequent read is decorated.
# Real files alone would miss the verify/restore bug after the first write.
cat >"$tmp/cat" <<'EOF'
#!/bin/sh
if [ "${LUNA_TEST_TSYNC_FORMAT:-plain}" = decorated ] &&
   [ "$#" = 1 ] && [ "$1" = "${LUNA_TSYNC_ENABLE:-}" ]; then
  value="$(/bin/cat "$1")" || exit 1
  case "$value" in
    0) printf '0: disabled\n'; exit 0 ;;
    1) printf '1: enabled\n'; exit 0 ;;
  esac
fi
exec /bin/cat "$@"
EOF
chmod +x "$tmp/cat"
export PATH="$tmp:$PATH"
export CALLS="$tmp/calls"
export KODI_STATE="$tmp/kodi-state" SESSION_STATE="$tmp/session-state"
export LUNA_LOG_DIR="$tmp/logs"
export MOONLIGHT_BINARY="$tmp/coreelec-ng/moonlight"
export LUNA_SYSTEMCTL="$tmp/systemctl"
export LUNA_SYSTEMD_RUN="$tmp/systemd-run"
export LUNA_KILLALL="$tmp/killall"
export LUNA_VIDEO_DISABLE="$tmp/disable-video"
export LUNA_VDEC_STATUS="$tmp/vdec-status"
export LUNA_FB0_BLANK="$tmp/fb0.blank" LUNA_FB1_BLANK="$tmp/fb1.blank"
export LUNA_CPUFREQ_ROOT="$tmp/cpufreq"
mkdir -p "$LUNA_CPUFREQ_ROOT/policy0" "$LUNA_CPUFREQ_ROOT/policy4"
printf '%s' schedutil > "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor"
printf '%s' ondemand > "$LUNA_CPUFREQ_ROOT/policy4/scaling_governor"
export LUNA_NET_CORE_ROOT="$tmp/net-core"
mkdir -p "$LUNA_NET_CORE_ROOT"
printf '%s' 212992 > "$LUNA_NET_CORE_ROOT/rmem_max"
printf '%s' 212992 > "$LUNA_NET_CORE_ROOT/rmem_default"
printf '%s' 1000 > "$LUNA_NET_CORE_ROOT/netdev_max_backlog"
export LUNA_AML_LOW_LATENCY="$tmp/aml-low"
export LUNA_AML_SKIP_POLICY="$tmp/aml-skip"
export LUNA_AML_REF_FLAGS="$tmp/aml-ref"
printf 0 > "$LUNA_AML_LOW_LATENCY"
printf 2 > "$LUNA_AML_SKIP_POLICY"
printf %s 1,1,1,1,1,1,1,1,1 > "$LUNA_AML_REF_FLAGS"
export LUNA_TSYNC_ENABLE="$tmp/tsync-enable"
printf '%s' 1 > "$LUNA_TSYNC_ENABLE"
printf 'active\n' > "$KODI_STATE"
printf 'active\n' > "$SESSION_STATE"
printf '1' > "$LUNA_FB0_BLANK"
printf '1' > "$LUNA_FB1_BLANK"
printf '0' > "$LUNA_VIDEO_DISABLE"
printf '%s\n' 'No vdec.' > "$LUNA_VDEC_STATUS"

launcher="$root/resources/launchscripts/coreelec/launch.sh"
watchdog="$root/resources/launchscripts/coreelec/kodi-watchdog.sh"

grep -Fxq '#!/bin/sh' "$launcher"
grep -Fxq '#!/bin/sh' "$watchdog"
grep -Fq 'watchdog_unit="luna-ng-watchdog-$$"' "$launcher"
grep -Fq '"/proc/$$/stat"' "$launcher"
grep -Fq 'luna_write_receipt "$session.owner" "$$|$owner_start"' "$launcher"
grep -Fq 'luna_write_receipt "$session.unit" "$SESSION_UNIT"' "$launcher"
grep -Fq -- '--property=Restart=on-failure' "$launcher"
grep -Fq -- '--property=RestartSec=30s' "$launcher"
grep -Fq 'restore_tuning_journal' "$launcher"
grep -Fq 'read_session_state' "$watchdog"
grep -Fq 'reconcile_pending_session' "$launcher"
grep -Fq '33554432' "$launcher"
grep -Fq '2097152' "$launcher"
grep -Fq 'netdev_max_backlog" 5000' "$launcher"
grep -Fq 'TSYNC_ENABLE="${LUNA_TSYNC_ENABLE:-/sys/class/tsync/enable}"' "$launcher"
grep -Fq 'disable_tsync' "$launcher"
grep -Fq 'restore_tsync' "$launcher"
grep -Fq '"$scripts/aml-recovery.sh" "$aml_state"' "$launcher"
grep -Fq 'display-recovery.sh' "$watchdog"
grep -Fq 'MOONLIGHT_READY_FILE="$stream_ready"' "$launcher"
grep -Fq 'grep -Fxq ready "$stream_ready"' "$launcher"
grep -Fq 'resumestate.py' "$launcher"
grep -Fq 'stream_confirmed=1' "$launcher"
grep -Fq 'recovery_lock="$session.recovery.lock"' "$launcher"
grep -Fq 'recovery_lock="$session.recovery.lock"' "$watchdog"
grep -Fq 'flock 9 ||' "$launcher"
grep -Fq 'flock -w 30 9' "$watchdog"
grep -Fq 'flock -w 45 9' "$watchdog"
grep -Fq 'stop --no-block "$session_unit"' "$watchdog"
! grep -Fq 'rm -f "$recovery_lock"' "$launcher"
! grep -Fq 'rmdir "$recovery_lock"' "$watchdog"
grep -Fq 'LUNA_STARTUP_RESULT phase=exit' "$launcher"
# An earlier armed recovery without a manifest must block a new launch
# rather than race still-unrestored sysfs state.
old_marker="$LUNA_LOG_DIR/.luna-session.old12345"
mkdir -p "$LUNA_LOG_DIR"
printf '1\n' > "$old_marker"
if sh "$launcher" Desktop >"$tmp/out" 2>&1; then
  echo 'Launcher ignored unresolved previous-session recovery' >&2
  exit 1
fi
grep -Fq 'manual recovery required' "$tmp/out"
rm -f "$old_marker"
# Verify the actual /proc fd ownership, including a negative control.
# A surviving child without fd 9 must not retain the owner's flock.
lockfile="$tmp/inheritance.lock"
readyfile="$tmp/owner-ready"
(
  exec 9>"$lockfile"
  flock 9
  sleep 4 9>&- &
  echo "$!" > "$tmp/closed-child"
  : > "$readyfile"
  sleep 10
) &
owner=$!
for _ in {1..100}; do [[ -e "$readyfile" ]] && break; sleep 0.01; done
[[ -e "$readyfile" ]]
closed_child="$(cat "$tmp/closed-child")"
[[ -d "/proc/$closed_child" && ! -e "/proc/$closed_child/fd/9" ]]
kill -KILL "$owner" 2>/dev/null || true
wait "$owner" 2>/dev/null || true
flock -w 1 "$lockfile" -c true
kill "$closed_child" 2>/dev/null || true

negative_lock="$tmp/inheritance-negative.lock"
(
  exec 9>"$negative_lock"
  flock 9
  sleep 4 &
  echo "$!" > "$tmp/open-child"
  : > "$tmp/negative-ready"
  sleep 10
) &
negative_owner=$!
for _ in {1..100}; do [[ -e "$tmp/negative-ready" ]] && break; sleep 0.01; done
[[ -e "$tmp/negative-ready" ]]
open_child="$(cat "$tmp/open-child")"
[[ -e "/proc/$open_child/fd/9" ]]
[[ "$(readlink "/proc/$open_child/fd/9")" == "$negative_lock" ]]
kill -KILL "$negative_owner" 2>/dev/null || true
wait "$negative_owner" 2>/dev/null || true
if flock -n "$negative_lock" -c true; then
  echo 'Inherited fd unexpectedly released lock while child is alive' >&2
  exit 1
fi
kill "$open_child" 2>/dev/null || true
for _ in {1..100}; do
  flock -n "$negative_lock" -c true && break
  sleep 0.01
done
flock -n "$negative_lock" -c true

# Kernel locks must serialize contenders and release after owner death.
lockfile="$tmp/recovery-concurrency.lock"
flock "$lockfile" -c 'sleep 0.2' &
lock_owner=$!
sleep 0.05
flock "$lockfile" sh -c 'printf acquired > "$1"' sh "$tmp/recovery-acquired" &
lock_waiter=$!
wait "$lock_owner"
wait "$lock_waiter"
[[ "$(cat "$tmp/recovery-acquired")" == acquired ]]
! grep -Fq '/dev/input/event' "$launcher"
! grep -Fq '/proc/"$child"/fd' "$launcher"
! grep -Fq 'LUNA_VFM' "$launcher"
! grep -Fq '/sys/class/vfm/map' "$launcher"
! grep -Fq 'ensure_ng_vfm_route' "$launcher"
! grep -Fq 'vfm_write' "$launcher"
sh -n "$launcher"
sh -n "$watchdog"
sh -n "$root/resources/launchscripts/coreelec/hang-diagnostics.sh"
grep -Fq 'hang-diagnostics.sh' "$watchdog"
grep -Fq 'luna_timeout -k 2 12 sh' "$watchdog"
grep -Fq '9>&-' "$launcher"
! grep -Eq '(kill |reboot|shutdown|sysrq-trigger)' "$root/resources/launchscripts/coreelec/hang-diagnostics.sh"

set +e
export LUNA_TEST_KODI_RESTART_SETTINGS="$LUNA_LOG_DIR/settings.xml"
sh "$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
[[ "$status" == 42 ]]
grep -Fq 'killall -KILL kodi.bin' "$CALLS"
grep -Fq 'systemctl stop kodi.service' "$CALLS"
grep -Fq 'moonlight stream -app Desktop' "$CALLS"
! grep -Fq 'moonlight_inherited_launch_lock' "$CALLS"
grep -Fq 'ready_file ' "$CALLS"
# Mock Moonlight exits immediately after signalling ready: connection remains confirmed.
grep -Fq 'LUNA_STARTUP_RESULT phase=exit' "$LUNA_LOG_DIR/logfile"
grep -Fq 'osd_takeover 1' "$CALLS"
grep -Fq '<setting id="last_run">Desktop</setting>' "$LUNA_LOG_DIR/settings.xml"
# Kodi restart must observe the already committed settings, not a later write.
# The systemctl stub verifies the XML synchronously when Kodi is restarted.
grep -Fq 'systemctl start kodi.service' "$CALLS"
grep -Fq 'luna_timeout -k 2 15 "$SYSTEMCTL" start "$KODI_UNIT"' "$launcher"
grep -Fq 'tsync 0' "$CALLS"
grep -Fq 'rmem_max 33554432' "$CALLS"
grep -Fq 'rmem_default 2097152' "$CALLS"
grep -Fq 'netdev_max_backlog 5000' "$CALLS"
grep -Fq "cwd $tmp/coreelec-ng" "$CALLS"
grep -Fq 'systemctl start kodi.service' "$CALLS"
grep -Fq -- '--unit=luna-ng-watchdog-' "$CALLS"
! grep -q -- '-STOP kodi.bin' "$CALLS"
! grep -q -- '-CONT kodi.bin' "$CALLS"
kill_line="$(grep -n -F 'killall -KILL kodi.bin' "$CALLS" | head -n1 | cut -d: -f1)"
stop_line="$(grep -n -F 'systemctl stop kodi.service' "$CALLS" | head -n1 | cut -d: -f1)"
[[ "$kill_line" -lt "$stop_line" ]]
[[ "$(cat "$LUNA_FB0_BLANK")" == 0 ]]
[[ "$(cat "$LUNA_FB1_BLANK")" == 0 ]]
[[ "$(cat "$LUNA_VIDEO_DISABLE")" == 1 ]]
[[ "$(cat "$KODI_STATE")" == active ]]
# The recovery lock inode intentionally survives; every other session file must be gone.
[[ -z "$(find "$tmp/logs" -name '.luna-session.*' ! -name '*.recovery.lock' -print)" ]]
while IFS= read -r lock; do
  flock -n "$lock" -c true || { echo "Leaked recovery lock: $lock" >&2; exit 1; }
done < <(find "$tmp/logs" -name '*.recovery.lock' -type f)
[[ "$(cat "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor")" == schedutil ]]
[[ "$(cat "$LUNA_CPUFREQ_ROOT/policy4/scaling_governor")" == ondemand ]]
[[ "$(cat "$LUNA_NET_CORE_ROOT/rmem_max")" == 212992 ]]
[[ "$(cat "$LUNA_NET_CORE_ROOT/rmem_default")" == 212992 ]]
[[ "$(cat "$LUNA_NET_CORE_ROOT/netdev_max_backlog")" == 1000 ]]
[[ "$(cat "$LUNA_TSYNC_ENABLE")" == 1 ]]
grep -Fq 'Amlogic tsync disabled for Moonlight' "$LUNA_LOG_DIR/logfile"
grep -Fq 'restored Amlogic tsync to 1' "$LUNA_LOG_DIR/logfile"
grep -Fq 'Skupi-style socket-buffer tuning enabled' "$LUNA_LOG_DIR/logfile"
# Value and recovery-marker assertions above verify restoration directly;
# success-log wording is not part of the recovery contract.

# Explicit session configurations must be passed to the native child and
# deleted after a completed session, not reread from shared moonlight.conf.
snapshot="$tmp/coreelec-ng/.luna-stream-test.conf"
printf '[Moonlight]\naddress = 192.0.2.20\n' > "$snapshot"
chmod 600 "$snapshot"
export LUNA_STREAM_CONFIG="$snapshot"
launch_ack="$tmp/coreelec-ng/.luna-ack-test.ack"
: > "$launch_ack"
chmod 600 "$launch_ack"
export LUNA_LAUNCH_ACK="$launch_ack"
: > "$CALLS"
printf 'active\n' > "$KODI_STATE"
printf '1' > "$LUNA_TSYNC_ENABLE"
set +e
sh "$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
unset LUNA_STREAM_CONFIG LUNA_LAUNCH_ACK
[[ "$status" == 42 ]]
grep -Fq "moonlight stream -config $snapshot -app Desktop" "$CALLS"
[[ ! -e "$snapshot" ]]
[[ ! -e "$launch_ack" ]]

# Reject external session paths without touching either the path or Kodi.
outside="$tmp/never-delete.conf"
printf 'sentinel' > "$outside"
export LUNA_STREAM_CONFIG="$outside"
: > "$CALLS"
set +e
sh "$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
unset LUNA_STREAM_CONFIG
[[ "$status" == 1 ]]
grep -Fq 'rejecting unsafe session configuration path' "$tmp/out"
[[ "$(cat "$outside")" == sentinel ]]
! grep -Fq 'killall -KILL kodi.bin' "$CALLS"

# CPU performance mode is opt-in, applies during Moonlight, and restores both
# policies after the stream exits.
: > "$CALLS"
printf 'active\n' > "$KODI_STATE"
printf '%s' 1 > "$LUNA_TSYNC_ENABLE"
printf '%s' schedutil > "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor"
printf '%s' ondemand > "$LUNA_CPUFREQ_ROOT/policy4/scaling_governor"
export MOONLIGHT_CPU_PERFORMANCE=1
export LUNA_TEST_TSYNC_FORMAT=decorated
set +e
unset LUNA_TEST_KODI_RESTART_SETTINGS
sh "$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
unset MOONLIGHT_CPU_PERFORMANCE
[[ "$status" == 42 ]]
grep -Fxq 'tsync 0: disabled' "$CALLS"
grep -Fq 'Amlogic tsync disabled for Moonlight' "$LUNA_LOG_DIR/logfile"
grep -Fq 'restored Amlogic tsync to 1' "$LUNA_LOG_DIR/logfile"
unset LUNA_TEST_TSYNC_FORMAT
[[ "$(cat "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor")" == schedutil ]]
[[ "$(cat "$LUNA_CPUFREQ_ROOT/policy4/scaling_governor")" == ondemand ]]
[[ "$(cat "$LUNA_TSYNC_ENABLE")" == 1 ]]
grep -Fq 'performance CPU governor enabled for 2 policy/policies' "$LUNA_LOG_DIR/logfile"
# The exact governor values and absence of pending journals are checked above.

# Already-disabled and unrecognized values are preserved without restore writes.
for initial in 0 '0: unexpected' '1: disabled'; do
  printf '%s' "$initial" > "$LUNA_TSYNC_ENABLE"
  export LUNA_TEST_TSYNC_FORMAT=decorated
  set +e
  sh "$launcher" Desktop >"$tmp/out" 2>&1
  status=$?
  set -e
  unset LUNA_TEST_TSYNC_FORMAT
  [[ "$status" == 42 ]]
  [[ "$(cat "$LUNA_TSYNC_ENABLE")" == "$initial" ]]
  if [[ "$initial" == 0 ]]; then
    grep -Fq 'Amlogic tsync already disabled' "$LUNA_LOG_DIR/logfile"
  else
    grep -Fq 'unexpected Amlogic tsync value; skipped' "$LUNA_LOG_DIR/logfile"
  fi
done
printf '%s' 1 > "$LUNA_TSYNC_ENABLE"

# Missing binary must fail before Kodi is stopped.
: > "$CALLS"
export MOONLIGHT_BINARY="$tmp/no-binary"
set +e
sh "$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
[[ "$status" == 127 ]]
! grep -Fq 'killall -KILL kodi.bin' "$CALLS"
! grep -Fq 'systemctl stop kodi.service' "$CALLS"
export MOONLIGHT_BINARY="$tmp/coreelec-ng/moonlight"

# Missing experimental components must restore Kodi without starting Moonlight,
# even when the launcher is invoked directly and bypasses Kodi's preflight.
: > "$CALLS"
printf 'active\n' > "$KODI_STATE"
export MOONLIGHT_VIDEO_PIPELINE=aml-vfm
export MOONLIGHT_VFM_ROOT="$tmp/missing-vfm"
set +e
sh "$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
unset MOONLIGHT_VIDEO_PIPELINE MOONLIGHT_VFM_ROOT
[[ "$status" == 1 ]]
grep -Fq 'systemctl stop kodi.service' "$CALLS"
grep -Fq 'systemctl start kodi.service' "$CALLS"
! grep -Fq 'moonlight stream' "$CALLS"
[[ "$(cat "$KODI_STATE")" == active ]]
grep -Fq 'prototype driver setup failed; restoring Kodi' "$LUNA_LOG_DIR/logfile"

# A failed Kodi stop cannot authorize writes against its active video plane.
: > "$CALLS"
printf 'active\n' > "$KODI_STATE"
printf '0' > "$LUNA_VIDEO_DISABLE"
export LUNA_TEST_KODI_STOP_FAIL=1
set +e
sh "$launcher" Desktop > "$tmp/out" 2>&1
status=$?
set -e
unset LUNA_TEST_KODI_STOP_FAIL
[[ "$status" == 1 ]]
[[ "$(cat "$KODI_STATE")" == active ]]
[[ "$(cat "$LUNA_VIDEO_DISABLE")" == 0 ]]
! grep -Fq 'moonlight stream -app Desktop' "$CALLS"
grep -Fq 'unable to complete stop' "$tmp/out"

# A native stream setup failure exits before the ready marker. Preserve the
# Moonlight status exactly, restore Kodi, and leave the previous Resume state.
python3 "$root/resources/lib/util/resumestate.py" set "$LUNA_LOG_DIR/settings.xml" Existing
: > "$CALLS"
printf 'active\n' > "$KODI_STATE"
printf '%s' 1 > "$LUNA_TSYNC_ENABLE"
export MOONLIGHT_TEST_CONNECTION_FAIL=1
set +e
sh "$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
unset MOONLIGHT_TEST_CONNECTION_FAIL
[[ "$status" == 23 ]]
grep -Fq 'systemctl stop kodi.service' "$CALLS"
grep -Fq 'systemctl start kodi.service' "$CALLS"
! grep -Fq 'ready_file ' "$CALLS"
grep -Fq '<setting id="last_run">Existing</setting>' "$LUNA_LOG_DIR/settings.xml"
! grep -Fq '<setting id="last_run">Desktop</setting>' "$LUNA_LOG_DIR/settings.xml"
grep -Fq 'Moonlight exited with status 23 before stream readiness; restoring Kodi' "$LUNA_LOG_DIR/logfile"
[[ "$(cat "$KODI_STATE")" == active ]]
[[ "$(cat "$LUNA_TSYNC_ENABLE")" == 1 ]]

# A Moonlight process that never reaches input/video initialization must not
# strand the user with Kodi stopped.
: > "$CALLS"
printf 'active\n' > "$KODI_STATE"
printf '%s' 1 > "$LUNA_TSYNC_ENABLE"
export MOONLIGHT_TEST_HANG=1
export LUNA_STARTUP_TIMEOUT=5
set +e
sh "$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
unset MOONLIGHT_TEST_HANG
unset LUNA_STARTUP_TIMEOUT
[[ "$status" == 124 ]]
grep -Fq 'systemctl stop kodi.service' "$CALLS"
grep -Fq 'systemctl start kodi.service' "$CALLS"
grep -Fq 'Moonlight startup timed out after 5s; restoring Kodi' "$LUNA_LOG_DIR/logfile"
grep -Fq '<setting id="last_run">Existing</setting>' "$LUNA_LOG_DIR/settings.xml"
[[ "$(cat "$KODI_STATE")" == active ]]
[[ "$(cat "$LUNA_TSYNC_ENABLE")" == 1 ]]

# Recovery worker restarts Kodi if an armed session disappears unexpectedly.
: > "$CALLS"
printf 'inactive\n' > "$KODI_STATE"
printf 'inactive\n' > "$SESSION_STATE"
printf '1' > "$LUNA_FB0_BLANK"
printf '1' > "$LUNA_FB1_BLANK"
session="$tmp/.luna-session.12345678"
export LUNA_TEST_SESSION_OWNER_FILE="$session.owner"
printf '1\n' > "$session"
printf 'not-started\n' > "$session.native-not-started"
printf 'ready\n' > "$session.stream-ready"
governor_state="$session.governor-state"
printf '%s|%s\n' "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor" schedutil > "$governor_state"
printf '%s' performance > "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor"
network_state="$session.network-state"
printf '%s|%s|%s\n' "$LUNA_NET_CORE_ROOT/rmem_max" 212992 33554432 > "$network_state"
printf '%s' 33554432 > "$LUNA_NET_CORE_ROOT/rmem_max"
tsync_state="$session.tsync-state"
printf '%s|%s|%s\n' "$LUNA_TSYNC_ENABLE" 1 0 > "$tsync_state"
printf '%s' 0 > "$LUNA_TSYNC_ENABLE"
export LUNA_TEST_TSYNC_FORMAT=decorated
"$watchdog" luna-ng-session.service "$session" "$governor_state" "$network_state" "$tsync_state"
unset LUNA_TEST_TSYNC_FORMAT
grep -Fq 'systemctl start kodi.service' "$CALLS"
[[ "$(cat "$KODI_STATE")" == active ]]
[[ "$(cat "$LUNA_FB0_BLANK")" == 0 ]]
[[ "$(cat "$LUNA_FB1_BLANK")" == 0 ]]
[[ "$(cat "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor")" == schedutil ]]
[[ "$(cat "$LUNA_NET_CORE_ROOT/rmem_max")" == 212992 ]]
[[ "$(cat "$LUNA_TSYNC_ENABLE")" == 1 ]]
[[ ! -e "$governor_state" ]]
[[ ! -e "$network_state" ]]
[[ ! -e "$tsync_state" ]]
[[ ! -e "$session" ]]
[[ ! -e "$session.ready" ]]
[[ ! -e "$session.stream-ready" ]]

# A value changed by another owner during recovery must not be overwritten.
printf '1\n' > "$session"
printf 'not-started\n' > "$session.native-not-started"
printf '%s|1|0\n' "$LUNA_TSYNC_ENABLE" > "$tsync_state"
printf '1: disabled' > "$LUNA_TSYNC_ENABLE"
if "$watchdog" luna-ng-session.service "$session" '' '' "$tsync_state" >"$tmp/out" 2>&1; then
  echo 'Watchdog discarded an unreadable externally modified tsync value' >&2; exit 1
fi
[[ "$(cat "$LUNA_TSYNC_ENABLE")" == '1: disabled' ]]
[[ -s "$tsync_state" && -e "$session" ]]
rm -f "$session" "$tsync_state" "$session.recovery.lock"

# Even if systemctl reports success, never disarm a Kodi service which
# immediately becomes inactive. Keep a durable marker and return failure.
cat >"$tmp/sleep" <<'EOF'
#!/bin/sh
if [ "${1:-}" = 2 ]; then exit 0; fi
exec /bin/sleep "$@"
EOF
chmod +x "$tmp/sleep"
: > "$CALLS"
printf 'inactive\n' > "$KODI_STATE"
printf 'inactive\n' > "$SESSION_STATE"
printf '1\n' > "$session"
printf 'not-started\n' > "$session.native-not-started"
printf 'ready\n' > "$session.stream-ready"
export LUNA_TEST_KODI_START_INACTIVE=1
if "$watchdog" luna-ng-session.service "$session" >"$tmp/out" 2>&1; then
  echo 'Watchdog falsely reported recovery despite Kodi being inactive' >&2
  exit 1
fi
unset LUNA_TEST_KODI_START_INACTIVE
[[ "$(/bin/cat "$session")" == 1 ]]
[[ ! -e "$session.stream-ready" ]]
[[ "$(/bin/cat "$KODI_STATE")" == inactive ]]
[[ "$(grep -c '^systemctl start kodi.service$' "$CALLS")" == 15 ]]
grep -Fq 'recovery remains armed' "$tmp/out"
rm -f "$tmp/sleep" "$session" "$session.recovery.lock"

# An actively running game must not time out. Only an explicit stale cleanup
# timestamp should trigger systemd TERM/KILL escalation and Kodi recovery.
: > "$CALLS"
printf 'inactive\n' > "$KODI_STATE"
printf 'active\n' > "$SESSION_STATE"
printf '1\n' > "$session"
printf 'not-started\n' > "$session.native-not-started"
printf '%s|%s\n' "$$" "$(sed 's/^.*) //' "/proc/$$/stat" | awk '{print $20}')" > "$session.owner"
printf '%s\n' "$(( $(date +%s) - 120 ))" > "$session.cleanup"
LUNA_CLEANUP_TIMEOUT=2 "$watchdog" luna-ng-session.service "$session" >"$tmp/out" 2>&1
grep -Fq "cleanup exceeded 2s" "$tmp/out"
grep -Fq "systemctl stop --no-block luna-ng-session.service" "$CALLS"
[[ "$(/bin/cat "$KODI_STATE")" == active ]]
[[ ! -e "$session.cleanup" ]]
printf 'inactive\n' > "$SESSION_STATE"

# A watchdog retry must not reset Amlogic video once Kodi is already active.
: > "$CALLS"
printf "active\n" > "$KODI_STATE"
printf "inactive\n" > "$SESSION_STATE"
printf "1\n" > "$session"
printf 'not-started\n' > "$session.native-not-started"
printf "0" > "$LUNA_VIDEO_DISABLE"
aml_pending="$session.aml-state"
printf "%s|2|0\n" "$tmp/temporarily-missing-aml-node" > "$aml_pending"
if "$watchdog" luna-ng-session.service "$session" "" "" "" "" "$aml_pending" >"$tmp/out" 2>&1; then
  echo "Watchdog discarded an incomplete AML recovery" >&2; exit 1
fi
[[ "$(/bin/cat "$LUNA_VIDEO_DISABLE")" == 0 ]]
[[ "$(/bin/cat "$KODI_STATE")" == active ]]
[[ "$(/bin/cat "$session")" == 1 ]]
[[ -s "$aml_pending" ]]
# Kodi was already active; video control above must remain untouched.
rm -f "$session" "$session.ready" "$session.recovery.lock" "$session.cleanup" "$aml_pending"

# A blocking systemctl start must have a bounded attempt and total recovery
# deadline; preserve the marker rather than waiting indefinitely.
: > "$CALLS"
printf "inactive\n" > "$KODI_STATE"
printf "inactive\n" > "$SESSION_STATE"
printf "1\n" > "$session"
printf 'not-started\n' > "$session.native-not-started"
export LUNA_TEST_KODI_BLOCK_START=1
if LUNA_RECOVERY_TIMEOUT=3 LUNA_RECOVERY_HELPER_TIMEOUT=1 \
   "$watchdog" luna-ng-session.service "$session" >"$tmp/out" 2>&1; then
  echo "Blocked Kodi start was not reported as recovery failure" >&2; exit 1
fi
unset LUNA_TEST_KODI_BLOCK_START
[[ "$(/bin/cat "$KODI_STATE")" == inactive ]]
[[ "$(/bin/cat "$session")" == 1 ]]
grep -Fq "recovery remains armed" "$tmp/out"
rm -f "$session" "$session.ready" "$session.recovery.lock" "$session.cleanup"

# Validate conservative AML restore behavior.
state="$tmp/aml-restore-state"
. "$root/resources/launchscripts/coreelec/aml-recovery.sh"
aml_snapshot "$state"
printf 1 > "$LUNA_AML_LOW_LATENCY"
printf 0 > "$LUNA_AML_SKIP_POLICY"
printf %s 0,0,0,0,0,0,0,0,0 > "$LUNA_AML_REF_FLAGS"
aml_restore "$state"
[[ "$(cat "$LUNA_AML_LOW_LATENCY")" == 0 ]]
[[ "$(cat "$LUNA_AML_SKIP_POLICY")" == 2 ]]
[[ "$(cat "$LUNA_AML_REF_FLAGS")" == 1,1,1,1,1,1,1,1,1 ]]
aml_snapshot "$state"
printf 1 > "$LUNA_AML_LOW_LATENCY"
printf 2 > "$LUNA_AML_SKIP_POLICY"
aml_restore "$state"
[[ "$(cat "$LUNA_AML_SKIP_POLICY")" == 2 ]]
# A startup helper that never returns must be killed by the independent
# watchdog even if Moonlight has not been launched and cleanup has not started.
: > "$CALLS"
printf 'inactive\n' > "$KODI_STATE"
printf 'active\n' > "$SESSION_STATE"
printf '1\n' > "$session"
printf 'not-started\n' > "$session.native-not-started"
printf '%s|%s\n' "$$" "$(sed 's/^.*) //' "/proc/$$/stat" | awk '{print $20}')" > "$session.owner"
printf '%s\n' "$(( $(date +%s) - 20 ))" > "$session.startup"
LUNA_STARTUP_TIMEOUT=5 "$watchdog" luna-ng-session.service "$session" >"$tmp/out" 2>&1
grep -Fq 'startup exceeded 5s' "$tmp/out"
grep -Fq 'systemctl stop --no-block luna-ng-session.service' "$CALLS"
[[ "$(/bin/cat "$KODI_STATE")" == active ]]
[[ ! -e "$session" && ! -e "$session.startup" ]]
printf 'inactive\n' > "$SESSION_STATE"

# A missing tsync control file must leave both journal and watchdog arm
# intact. A later retry must restore the recorded value and disarm safely.
: > "$CALLS"
printf 'active\n' > "$KODI_STATE"
printf '1\n' > "$session"
printf 'not-started\n' > "$session.native-not-started"
# Exercise a temporarily absent TRUSTED tsync node, not an unrelated
# filesystem path. The recovery implementation must reject the latter.
tsync_missing="$LUNA_TSYNC_ENABLE"
rm -f "$tsync_missing"
printf '%s|1|0\n' "$tsync_missing" > "$tsync_state"
if "$watchdog" luna-ng-session.service "$session" '' '' "$tsync_state" >"$tmp/out" 2>&1; then
  echo 'Watchdog falsely reported success with unrestored tsync' >&2; exit 1
fi
[[ -s "$tsync_state" && -e "$session" ]]
printf '0' > "$tsync_missing"
"$watchdog" luna-ng-session.service "$session" '' '' "$tsync_state"
[[ "$(/bin/cat "$tsync_missing")" == 1 ]]
[[ ! -e "$tsync_state" && ! -e "$session" ]]

echo 'PASS: Kodi handoff, bounded pre-stream startup, tsync/HDMI recovery journals, and crash recovery'
