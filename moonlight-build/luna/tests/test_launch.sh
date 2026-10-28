#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

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
if [ "${MOONLIGHT_TEST_HANG:-0}" = 1 ]; then
  trap 'exit 0' TERM
  while :; do sleep 1; done
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
    printf 'active\n' > "$KODI_STATE"
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
    */.luna-session.*) session="$arg" ;;
  esac
done
[ -n "$session" ] || exit 2
: > "$session.ready"
EOF

chmod +x "$tmp/coreelec-ng/moonlight" "$tmp/killall" "$tmp/systemctl" "$tmp/systemd-run"
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
! grep -Fq 'LUNA_VFM' "$launcher"
! grep -Fq '/sys/class/vfm/map' "$launcher"
! grep -Fq 'ensure_ng_vfm_route' "$launcher"
! grep -Fq 'vfm_write' "$launcher"
sh -n "$launcher"
sh -n "$watchdog"

set +e
"$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
[[ "$status" == 42 ]]
grep -Fq 'killall -KILL kodi.bin' "$CALLS"
grep -Fq 'systemctl stop kodi.service' "$CALLS"
grep -Fq 'moonlight stream -app Desktop' "$CALLS"
grep -Fq 'osd_takeover 1' "$CALLS"
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
[[ -z "$(find "$tmp/logs" -name '.luna-session.*' -print)" ]]
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
set +e
"$launcher" Desktop >"$tmp/out" 2>&1
status=$?
set -e
unset MOONLIGHT_CPU_PERFORMANCE
[[ "$status" == 42 ]]
[[ "$(cat "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor")" == schedutil ]]
[[ "$(cat "$LUNA_CPUFREQ_ROOT/policy4/scaling_governor")" == ondemand ]]
[[ "$(cat "$LUNA_TSYNC_ENABLE")" == 1 ]]
grep -Fq 'performance CPU governor enabled for 2 policy/policies' "$LUNA_LOG_DIR/logfile"
grep -Fq 'restored CPU governor' "$LUNA_LOG_DIR/logfile"

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
governor_state="$tmp/recovery-governor"
printf '%s|%s\n' "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor" schedutil > "$governor_state"
printf '%s' performance > "$LUNA_CPUFREQ_ROOT/policy0/scaling_governor"
network_state="$tmp/recovery-network"
printf '%s|%s|%s\n' "$LUNA_NET_CORE_ROOT/rmem_max" 212992 33554432 > "$network_state"
printf '%s' 33554432 > "$LUNA_NET_CORE_ROOT/rmem_max"
tsync_state="$tmp/recovery-tsync"
printf '%s|%s|%s\n' "$LUNA_TSYNC_ENABLE" 1 0 > "$tsync_state"
printf '%s' 0 > "$LUNA_TSYNC_ENABLE"
"$watchdog" luna-ng-session.service "$session" "$governor_state" "$network_state" "$tsync_state"
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

echo 'PASS: Kodi handoff, tsync/socket-buffer/CPU restore, startup timeout recovery, and crash recovery'
