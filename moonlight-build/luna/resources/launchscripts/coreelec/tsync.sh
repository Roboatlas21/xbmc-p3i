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
