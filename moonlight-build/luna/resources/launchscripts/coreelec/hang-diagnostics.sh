#!/bin/sh
# Diagnostic-only recovery failure capture; no signals, sysfs writes or reboot.
set -u
scripts="$(CDPATH='' cd "$(dirname "$0")" && pwd)" || exit 1
. "$scripts/kodi-state.sh" || exit 126
[ "$#" -eq 2 ] || exit 2
unit="$1"
session="$2"
case "$unit" in ''|*[!A-Za-z0-9_.@-]*) exit 2 ;; esac
logdir="${LUNA_LOG_DIR:-/storage/.kodi/userdata/addon_data/script.luna}"
mkdir -p "$logdir" || exit 1
out="$logdir/hang-diagnostics-$(date -u +%Y%m%dT%H%M%SZ)-$$.txt"
umask 077
{
  echo "Luna NG recovery timeout diagnostics"
  date -u
  echo "unit=$unit"
  echo "session=$session"
  echo "--- systemd unit ---"
  luna_timeout 3 systemctl show "$unit" --property=ActiveState,SubState,MainPID,ControlGroup,Result 2>&1 || true
  echo "--- relevant processes ---"
  for status in /proc/[0-9]*/status; do
    pid="${status#/proc/}"; pid="${pid%/status}"
    exe="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
    case "$exe" in */moonlight|*/kodi.bin|*/sh|*/busybox) ;; *) continue ;; esac
    # Only include shell processes belonging to this session or watchdog.
    case "$exe" in */sh|*/busybox)
      cmd="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
      case "$cmd" in *kodi-watchdog.sh*|*coreelec/launch.sh*) ;; *) continue ;; esac
      ;;
    esac
    echo "PID=$pid exe=$exe"
    grep -E '^(Name|State|Pid|PPid|Threads):' "$status" 2>/dev/null || true
    printf 'wchan='
    cat "/proc/$pid/wchan" 2>/dev/null || true
    echo
    echo "kernel stack:"
    head -n 40 "/proc/$pid/stack" 2>/dev/null || true
  done
  echo "--- recent kernel messages ---"
  luna_timeout 3 dmesg 2>&1 | tail -n 100 || true
} > "$out" 2>&1
echo "Luna NG hang diagnostics saved: $out"
