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
SESSION_UNIT="${LUNA_SESSION_UNIT:-luna-ng-session.service}"
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
bin_dir=''
case "$BIN" in
  */*)
    bin_dir="$(CDPATH='' cd "$(dirname "$BIN")" && pwd)" || {
      echo "Moonlight binary directory is unavailable: $BIN" >&2
      exit 127
    }
    ;;
esac
if [ ! -r "$scripts/kodi-watchdog.sh" ]; then
  echo "Missing Kodi recovery watchdog" >&2
  exit 126
fi
resume_helper="$scripts/../../lib/util/resumestate.py"
if [ ! -r "$resume_helper" ]; then
  echo "Missing Luna resume-state helper" >&2
  exit 126
fi

session="$(mktemp "$(dirname "$LOG")/.luna-session.XXXXXXXX")" || exit 1
printf '0\n' > "$session" || exit 1
governor_state="$(mktemp "$(dirname "$LOG")/.luna-governor.XXXXXXXX")" || {
  rm -f "$session"
  exit 1
}
: > "$governor_state"
network_state="$(mktemp "$(dirname "$LOG")/.luna-network.XXXXXXXX")" || {
  rm -f "$session" "$governor_state"
  exit 1
}
: > "$network_state"
tsync_state="$(mktemp "$(dirname "$LOG")/.luna-tsync.XXXXXXXX")" || {
  rm -f "$session" "$governor_state" "$network_state"
  exit 1
}
: > "$tsync_state"
hdmi_state="$(mktemp "$(dirname "$LOG")/.luna-hdmi.XXXXXXXX")" || {
  rm -f "$session" "$governor_state" "$network_state" "$tsync_state"
  exit 1
}
stream_ready="$session.stream-ready"
if ! : > "$stream_ready"; then
  rm -f "$session" "$governor_state" "$network_state" "$tsync_state" "$hdmi_state"
  exit 1
fi
watchdog_unit="luna-ng-watchdog-$$"
child=''
kodi_stopped=0

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
        continue
        ;;
    esac
    [ "$original" = performance ] && continue

    if ! printf '%s' performance > "$governor" 2>/dev/null; then
      echo "Luna NG: failed to set performance governor: $governor" >> "$LOG"
      continue
    fi
    current="$(cat "$governor" 2>/dev/null)" || current=''
    if [ "$current" != performance ]; then
      echo "Luna NG: performance governor was not confirmed: $governor" >> "$LOG"
      printf '%s' "$original" > "$governor" 2>/dev/null || true
      continue
    fi

    printf '%s|%s\n' "$governor" "$original" >> "$governor_state"
    changed=$((changed + 1))
  done

  if [ "$changed" -gt 0 ]; then
    echo "Luna NG: performance CPU governor enabled for $changed policy/policies" >> "$LOG"
  fi
}

restore_cpu_governors() {
  [ -e "$governor_state" ] || return 0

  if [ -s "$governor_state" ]; then
    while IFS='|' read -r governor original; do
      [ -n "$governor" ] && [ -n "$original" ] || continue
      [ -r "$governor" ] && [ -w "$governor" ] || {
        echo "Luna NG: cannot restore CPU governor: $governor" >> "$LOG"
        continue
      }

      current="$(cat "$governor" 2>/dev/null)" || current=''
      if [ "$current" != performance ]; then
        echo "Luna NG: CPU governor changed externally; not overwriting $governor" >> "$LOG"
        continue
      fi

      if printf '%s' "$original" > "$governor" 2>/dev/null; then
        echo "Luna NG: restored CPU governor $governor to $original" >> "$LOG"
      else
        echo "Luna NG: failed restoring CPU governor $governor to $original" >> "$LOG"
      fi
    done < "$governor_state"
  fi
  rm -f "$governor_state"
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
      return 0
      ;;
  esac

  # Treat Skupi's values as minimums so a larger existing configuration is
  # never reduced for Moonlight.
  if [ "$original" -ge "$target" ]; then
    return 0
  fi

  if ! printf '%s' "$target" > "$path" 2>/dev/null; then
    echo "Luna NG: failed to tune network value: $path" >> "$LOG"
    return 0
  fi

  current="$(cat "$path" 2>/dev/null)" || current=''
  if [ "$current" != "$target" ]; then
    echo "Luna NG: network tuning was not confirmed: $path" >> "$LOG"
    printf '%s' "$original" > "$path" 2>/dev/null || true
    return 0
  fi

  printf '%s|%s|%s\n' "$path" "$original" "$target" >> "$network_state"
}

enable_network_tuning() {
  tune_network_value "$NET_CORE_ROOT/rmem_max" 33554432
  tune_network_value "$NET_CORE_ROOT/rmem_default" 2097152
  tune_network_value "$NET_CORE_ROOT/netdev_max_backlog" 5000

  if [ -s "$network_state" ]; then
    echo "Luna NG: Skupi-style socket-buffer tuning enabled" >> "$LOG"
  fi
}

restore_network_tuning() {
  [ -e "$network_state" ] || return 0

  if [ -s "$network_state" ]; then
    while IFS='|' read -r path original target; do
      [ -n "$path" ] && [ -n "$original" ] && [ -n "$target" ] || continue
      [ -r "$path" ] && [ -w "$path" ] || {
        echo "Luna NG: cannot restore network value: $path" >> "$LOG"
        continue
      }

      current="$(cat "$path" 2>/dev/null)" || current=''
      if [ "$current" != "$target" ]; then
        echo "Luna NG: network value changed externally; not overwriting $path" >> "$LOG"
        continue
      fi

      if printf '%s' "$original" > "$path" 2>/dev/null; then
        echo "Luna NG: restored network value $path to $original" >> "$LOG"
      else
        echo "Luna NG: failed restoring network value $path to $original" >> "$LOG"
      fi
    done < "$network_state"
  fi
  rm -f "$network_state"
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

  # If tsync was already disabled, preserve that state and avoid an unnecessary
  # restore write when Moonlight exits.
  [ "$original" = 0 ] && {
    echo "Luna NG: Amlogic tsync already disabled" >> "$LOG"
    return 0
  }

  if ! printf '%s' 0 > "$TSYNC_ENABLE" 2>/dev/null; then
    echo "Luna NG: failed to disable Amlogic tsync" >> "$LOG"
    return 0
  fi

  printf '%s|%s|0\n' "$TSYNC_ENABLE" "$original" > "$tsync_state"
  current="$(read_tsync "$TSYNC_ENABLE")" || current=''
  if [ "$current" != 0 ]; then
    echo "Luna NG: Amlogic tsync disable was not confirmed; restoring" >> "$LOG"
    restore_tsync
    return 0
  fi

  echo "Luna NG: Amlogic tsync disabled for Moonlight" >> "$LOG"
}

restore_tsync() {
  [ -e "$tsync_state" ] || return 0

  if [ -s "$tsync_state" ]; then
    IFS='|' read -r path original applied < "$tsync_state" || true
    if [ -n "${path:-}" ] && [ -n "${original:-}" ] && [ -n "${applied:-}" ] &&
       [ -r "$path" ] && [ -w "$path" ]; then
      current="$(read_tsync "$path")" || current=''
      if [ "$current" = "$applied" ]; then
        if printf '%s' "$original" > "$path" 2>/dev/null; then
          verify="$(read_tsync "$path")" || verify=''
          if [ "$verify" = "$original" ]; then
            echo "Luna NG: restored Amlogic tsync to $original" >> "$LOG"
          else
            echo "Luna NG: Amlogic tsync restore was not confirmed" >> "$LOG"
          fi
        else
          echo "Luna NG: failed restoring Amlogic tsync to $original" >> "$LOG"
        fi
      else
        echo "Luna NG: Amlogic tsync changed externally; not overwriting" >> "$LOG"
      fi
    else
      echo "Luna NG: cannot restore Amlogic tsync" >> "$LOG"
    fi
  fi
  rm -f "$tsync_state"
}

restore_kodi_framebuffers() {
  for blank in "${LUNA_FB0_BLANK:-/sys/class/graphics/fb0/blank}" \
               "${LUNA_FB1_BLANK:-/sys/class/graphics/fb1/blank}"; do
    [ -e "$blank" ] || continue
    if [ -w "$blank" ]; then
      printf '0' > "$blank" 2>/dev/null || \
        echo "Failed to unblank Kodi framebuffer: $blank" >&2
    else
      echo "Kodi framebuffer blank control is not writable: $blank" >&2
    fi
  done
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
  [ -n "$BIN" ] || return 0
  case "$BIN" in /*) expected="$BIN" ;; *) expected="$(command -v "$BIN" 2>/dev/null || printf '%s' "$BIN")" ;; esac

  found=0
  for exe in /proc/[0-9]*/exe; do
    target="$(readlink "$exe" 2>/dev/null)" || continue
    [ "$target" = "$expected" ] || continue
    pid="${exe#/proc/}"
    pid="${pid%/exe}"
    [ "$pid" = "$" ] && continue
    found=1
    echo "Luna NG: terminating orphaned Moonlight pid $pid before Kodi restart" >> "$LOG"
    kill -TERM "$pid" 2>/dev/null || true
  done

  [ "$found" -eq 1 ] || return 0
  sleep 1

  for exe in /proc/[0-9]*/exe; do
    target="$(readlink "$exe" 2>/dev/null)" || continue
    [ "$target" = "$expected" ] || continue
    pid="${exe#/proc/}"
    pid="${pid%/exe}"
    [ "$pid" = "$" ] && continue
    echo "Luna NG: force-killing orphaned Moonlight pid $pid" >> "$LOG"
    kill -KILL "$pid" 2>/dev/null || true
  done
}

reset_moonlight_video_plane() {
  if [ -e "$VIDEO_DISABLE" ]; then
    if [ -w "$VIDEO_DISABLE" ]; then
      printf '1' > "$VIDEO_DISABLE" 2>/dev/null ||         echo "Luna NG: failed to disable Amlogic video plane" >> "$LOG"
    else
      echo "Luna NG: Amlogic video disable control is not writable" >> "$LOG"
    fi
  fi

  if [ -r "$VDEC_STATUS" ] && ! grep -Fq 'No vdec.' "$VDEC_STATUS" 2>/dev/null; then
    attempt=0
    while [ "$attempt" -lt 3 ]; do
      sleep 1
      grep -Fq 'No vdec.' "$VDEC_STATUS" 2>/dev/null && return 0
      attempt=$((attempt + 1))
    done
    echo "Luna NG: decoder still active after Moonlight teardown" >> "$LOG"
  fi
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

  if [ -n "$child" ] && kill -0 "$child" 2>/dev/null; then
    kill -TERM "$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
  fi

  # Do not return display ownership to Kodi while an exact copy of Luna's
  # Moonlight binary is still alive. This catches descendants that escaped
  # the shell's direct child PID bookkeeping.
  terminate_orphaned_moonlight

  python3 "$scripts/../../lib/util/hdmigamemode.py" restore "$hdmi_state" >> "$LOG" 2>&1 || true
  restore_tsync
  restore_network_tuning
  restore_cpu_governors

  # CoreELEC NG's idle state has amvideo disabled. Moonlight enables it for
  # direct VPU output; restore the idle state even if native teardown failed.
  reset_moonlight_video_plane
  restore_kodi_framebuffers

  if [ "$kodi_stopped" -eq 1 ]; then
    if "$SYSTEMCTL" start "$KODI_UNIT"; then
      printf '0\n' > "$session" 2>/dev/null || true
      rm -f "$session" "$session.ready"
    else
      echo "Luna NG: Kodi restart failed; recovery watchdog remains armed" >&2
    fi
  else
    rm -f "$session" "$session.ready"
  fi
  rm -f "$governor_state" "$network_state" "$tsync_state" "$stream_ready"
  [ -s "$hdmi_state" ] || rm -f "$hdmi_state"

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
    /usr/bin/env \
      "LUNA_SYSTEMCTL=$SYSTEMCTL" \
      "LUNA_KODI_UNIT=$KODI_UNIT" \
      "LUNA_FB0_BLANK=${LUNA_FB0_BLANK:-/sys/class/graphics/fb0/blank}" \
      "LUNA_FB1_BLANK=${LUNA_FB1_BLANK:-/sys/class/graphics/fb1/blank}" \
      /bin/sh "$scripts/kodi-watchdog.sh" "$SESSION_UNIT" "$session" "$governor_state" "$network_state" "$tsync_state" "$hdmi_state"; then
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

if "$SYSTEMCTL" is-active --quiet "$KODI_UNIT"; then
  kodi_stopped=1
  printf '1\n' > "$session"

  # Do not let Kodi perform its normal application shutdown here. A graceful
  # kodi.bin exit can execute the user's CEC shutdown action and put the TV
  # into standby just because Luna is starting. Kill only kodi.bin first so
  # evdev/audio/CEC resources are released without Kodi's shutdown callbacks,
  # then stop the wrapper service immediately to cancel Restart=always.
  "$KILLALL" -KILL kodi.bin 2>/dev/null || true

  if ! "$SYSTEMCTL" stop "$KODI_UNIT"; then
    echo "Luna NG: failed to stop $KODI_UNIT" >&2
    exit 1
  fi

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
  if ! python3 "$script_dir/../../lib/util/vfmruntime.py" load "$MOONLIGHT_VFM_ROOT" >> "$LOG" 2>&1; then
    echo "AML-VFM: prototype driver setup failed; restoring Kodi" >> "$LOG"
    exit 1
  fi
fi

disable_tsync
enable_network_tuning
enable_cpu_performance_governor

if [ "${MOONLIGHT_TV_GAME_MODE:-0}" = 1 ]; then
  python3 "$scripts/../../lib/util/hdmigamemode.py" enable "$hdmi_state" "$HDMI_ROOT" >> "$LOG" 2>&1 || true
fi

if [ -n "$bin_dir" ]; then
  cd "$bin_dir" || {
    echo "Unable to enter Moonlight binary directory: $bin_dir" >&2
    exit 1
  }
fi

MOONLIGHT_READY_FILE="$stream_ready" "$BIN" stream -app "$@" >> "$LOG" 2>&1 &
child=$!

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
  if ! python3 "$resume_helper" set "$RESUME_SETTINGS" "$1" >> "$LOG" 2>&1; then
    echo "Luna NG: failed to persist confirmed Resume state" >> "$LOG"
  fi
fi

wait "$child"
child_status=$?
if [ "$child_status" -ne 0 ] && ! startup_ready; then
  echo "Luna NG: Moonlight exited with status ${child_status} before stream readiness; restoring Kodi" >> "$LOG"
fi
exit "$child_status"
