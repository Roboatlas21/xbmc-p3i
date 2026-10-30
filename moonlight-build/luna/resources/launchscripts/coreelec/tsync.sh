#!/bin/sh
# Shared by the launcher and its independent crash-recovery watchdog.
# CE21's vendor driver reads as "0: disabled" / "1: enabled" but accepts
# numeric writes. Other kernels expose the numeric value directly.
read_tsync() {
  tsync_value="$(cat "$1" 2>/dev/null)" || return 1
  case "$tsync_value" in
    0|'0: disabled') printf '0' ;;
    1|'1: enabled') printf '1' ;;
    *) return 1 ;;
  esac
}

# The journal is removed only once the original tsync value was confirmed.
restore_tsync_snapshot() {
  record="$1"
  [ -e "$record" ] || return 0
  if [ ! -s "$record" ]; then rm -f "$record"; return $?; fi
  IFS='|' read -r path original applied < "$record" || return 1
  case "${original:-}:${applied:-}" in 0:0|0:1|1:0|1:1) ;; *)
    echo "Luna NG: invalid tsync recovery record" >&2; return 1 ;;
  esac
  if [ "${path:-}" != "${LUNA_TSYNC_ENABLE:-/sys/class/tsync/enable}" ]; then
    echo 'Luna NG: refusing untrusted tsync recovery destination' >&2
    return 1
  fi
  [ -r "$path" ] && [ -w "$path" ] || {
    echo "Luna NG: tsync node unavailable; keeping recovery record" >&2
    return 1
  }
  current="$(read_tsync "$path")" || {
    echo "Luna NG: tsync read failed; keeping recovery record" >&2
    return 1
  }
  if [ "$current" != "$original" ]; then
    if [ "$current" != "$applied" ]; then
      echo "Luna NG: tsync changed externally; not overwriting" >&2
      rm -f "$record"
      return $?
    fi
    if ! printf '%s' "$original" > "$path" 2>/dev/null; then
      echo "Luna NG: tsync restore write failed; keeping recovery record" >&2
      return 1
    fi
    verified="$(read_tsync "$path")" || verified=''
    if [ "$verified" != "$original" ]; then
      echo "Luna NG: tsync restore unconfirmed; keeping recovery record" >&2
      return 1
    fi
  fi
  echo "Luna NG: restored Amlogic tsync to $original" >&2
  rm -f "$record"
}
