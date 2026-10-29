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
  stop)
    printf 'systemctl stop %s\n' "$2" >> "$CALLS"
    printf 'inactive\n' > "$KODI_STATE"
    ;;
  start)
    printf 'systemctl start %s\n' "$2" >> "$CALLS"
    if [ "${LUNA_TEST_KODI_RESTART_FAIL:-0}" = 1 ]; then exit 1; fi
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
grep -Fq '33554432' "$launcher"
grep -Fq '2097152' "$launcher"
grep -Fq 'netdev_max_backlog" 5000' "$launcher"
grep -Fq 'TSYNC_ENABLE="${LUNA_TSYNC_ENABLE:-/sys/class/tsync/enable}"' "$launcher"
grep -Fq 'disable_tsync' "$launcher"
grep -Fq 'restore_tsync' "$launcher"
grep -Fq 'aml_restore "$aml_state"' "$launcher"
grep -Fq 'aml_reset_video' "$watchdog"
grep -Fq 'MOONLIGHT_READY_FILE="$stream_ready"' "$launcher"
grep -Fq 'grep -Fxq ready "$stream_ready"' "$launcher"
grep -Fq 'resumestate.py' "$launcher"
grep -Fq 'stream_confirmed=1' "$launcher"
grep -Fq 'recovery_lock="$session.recovery.lock"' "$launcher"
grep -Fq 'recovery_lock="$session.recovery.lock"' "$watchdog"
grep -Fq 'flock 9 ||' "$launcher"
grep -Fq 'flock -w 30 9' "$watchdog"
grep -Fq 'flock -w 45 9' "$watchdog"
grep -Fq '"$SYSTEMCTL" stop --no-block "$session_unit"' "$watchdog"
! grep -Fq 'rm -f "$recovery_lock"' "$launcher"
! grep -Fq 'rmdir "$recovery_lock"' "$watchdog"
grep -Fq 'LUNA_STARTUP_RESULT phase=exit' "$launcher"
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
grep -Fq 'timeout -k 2 12 sh' "$watchdog"
grep -Fq '9>&-' "$launcher"
! grep -Eq '(kill |reboot|shutdown|sysrq-trigger)' "$root/resources/launchscripts/coreelec/hang-diagnostics.sh"

set +e
export LUNA_TEST_KODI_RESTART_SETTINGS="$LUNA_LOG_DIR/settings.xml"
"$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
[[ "$status" == 42 ]]
grep -Fq 'killall -KILL kodi.bin' "$CALLS"
grep -Fq 'systemctl stop kodi.service' "$CALLS"
grep -Fq 'moonlight stream -app Desktop' "$CALLS"
grep -Fq 'ready_file ' "$CALLS"
# Mock Moonlight exits immediately after signalling ready: connection remains confirmed.
grep -Fq 'LUNA_STARTUP_RESULT phase=exit' "$LUNA_LOG_DIR/logfile"
grep -Fq 'osd_takeover 1' "$CALLS"
grep -Fq '<setting id="last_run">Desktop</setting>' "$LUNA_LOG_DIR/settings.xml"
# Kodi restart must observe the already committed settings, not a later write.
# The systemctl stub verifies the XML synchronously when Kodi is restarted.
grep -Fq 'systemctl start kodi.service' "$CALLS"
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
grep -Fq 'restored network value' "$LUNA_LOG_DIR/logfile"

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
"$launcher" Desktop >"$tmp/out" 2>&1
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
grep -Fq 'restored CPU governor' "$LUNA_LOG_DIR/logfile"

# Already-disabled and unrecognized values are preserved without restore writes.
for initial in 0 '0: unexpected' '1: disabled'; do
  printf '%s' "$initial" > "$LUNA_TSYNC_ENABLE"
  export LUNA_TEST_TSYNC_FORMAT=decorated
  set +e
  "$launcher" Desktop >"$tmp/out" 2>&1
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
"$launcher" Desktop >"$tmp/out" 2>&1
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
"$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
unset MOONLIGHT_VIDEO_PIPELINE MOONLIGHT_VFM_ROOT
[[ "$status" == 1 ]]
grep -Fq 'systemctl stop kodi.service' "$CALLS"
grep -Fq 'systemctl start kodi.service' "$CALLS"
! grep -Fq 'moonlight stream' "$CALLS"
[[ "$(cat "$KODI_STATE")" == active ]]
grep -Fq 'prototype driver setup failed; restoring Kodi' "$LUNA_LOG_DIR/logfile"

# A native stream setup failure exits before the ready marker. Preserve the
# Moonlight status exactly, restore Kodi, and leave the previous Resume state.
python3 "$root/resources/lib/util/resumestate.py" set "$LUNA_LOG_DIR/settings.xml" Existing
: > "$CALLS"
printf 'active\n' > "$KODI_STATE"
printf '%s' 1 > "$LUNA_TSYNC_ENABLE"
export MOONLIGHT_TEST_CONNECTION_FAIL=1
set +e
"$launcher" Desktop >"$tmp/out" 2>&1
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
"$launcher" Desktop >"$tmp/out" 2>&1
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
session="$tmp/recovery-session"
printf '1\n' > "$session"
printf 'ready\n' > "$session.stream-ready"
governor_state="$tmp/recovery-governor"
printf '%s|%s\n' "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor" schedutil > "$governor_state"
printf '%s' performance > "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor"
network_state="$tmp/recovery-network"
printf '%s|%s|%s\n' "$LUNA_NET_CORE_ROOT/rmem_max" 212992 33554432 > "$network_state"
printf '%s' 33554432 > "$LUNA_NET_CORE_ROOT/rmem_max"
tsync_state="$tmp/recovery-tsync"
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
printf '%s|1|0\n' "$LUNA_TSYNC_ENABLE" > "$tsync_state"
printf '1: disabled' > "$LUNA_TSYNC_ENABLE"
"$watchdog" luna-ng-session.service "$session" '' '' "$tsync_state"
[[ "$(cat "$LUNA_TSYNC_ENABLE")" == '1: disabled' ]]

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
echo 'PASS: Kodi handoff, tsync/socket-buffer/CPU restore, startup timeout recovery, and crash recovery'
