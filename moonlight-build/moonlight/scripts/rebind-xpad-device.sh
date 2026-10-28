#!/bin/sh
# Opt-in xpad-only recovery on a test device. Never called automatically.
set -eu
if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
  echo "Usage: $0 /dev/input/eventN XXXX YYYY [--apply]" >&2
  exit 2
fi
event="$1"
vendor="$(printf '%s' "$2" | tr 'A-F' 'a-f')"
product="$(printf '%s' "$3" | tr 'A-F' 'a-f')"
mode="${4:---dry-run}"
if ! printf '%s' "$event" | grep -Eq '^/dev/input/event[0-9]+$' ||
   ! printf '%s' "$vendor" | grep -Eq '^[0-9a-f]{4}$' ||
   ! printf '%s' "$product" | grep -Eq '^[0-9a-f]{4}$'; then
  echo "Invalid event or exact hexadecimal USB vendor/product ID" >&2
  exit 2
fi
case "$mode" in --dry-run|--apply) ;; *) echo "Invalid mode" >&2; exit 2;; esac

root="${MOONLIGHT_XPAD_SYSROOT:-/sys}"
if [ "$root" != /sys ] && [ "${MOONLIGHT_XPAD_TEST_ONLY:-}" != 1 ]; then
  echo "Alternate sysroot only permitted in isolated tests" >&2
  exit 2
fi
root="${root%/}"
event_name="${event##*/}"
device="$(readlink -f "$root/class/input/$event_name/device" 2>/dev/null || true)"
xpad="$(readlink -f "$root/bus/usb/drivers/xpad" 2>/dev/null || true)"
if [ ! -d "$device" ] || [ ! -d "$xpad" ]; then
  echo "Selected input event node or xpad driver unavailable" >&2
  exit 1
fi
case "$device" in "$root"/devices/*) ;; *)
  echo "Unexpected sysfs ancestry, refusing to change any driver" >&2
  exit 1;;
esac

current="$device"; iface=""; step=0
while [ "$step" -lt 12 ]; do
  if [ -L "$current/driver" ] &&
     [ "$(readlink -f "$current/driver")" = "$xpad" ]; then
    iface="$current"
    break
  fi
  parent="${current%/*}"
  [ "$parent" != "$current" ] && [ "$parent" != "$root" ] || break
  current="$parent"
  step=$((step + 1))
done
if [ -z "$iface" ]; then
  echo "Selected event is not bound to xpad; refusing recovery" >&2
  exit 1
fi

current="$iface"; verified=0; step=0
while [ "$step" -lt 10 ]; do
  if [ -r "$current/idVendor" ] && [ -r "$current/idProduct" ]; then
    actual_vendor="$(tr 'A-F' 'a-f' < "$current/idVendor" | tr -d '\n')"
    actual_product="$(tr 'A-F' 'a-f' < "$current/idProduct" | tr -d '\n')"
    if [ "$actual_vendor" != "$vendor" ] || [ "$actual_product" != "$product" ]; then
      echo "USB VID:PID mismatch; refusing recovery" >&2
      exit 1
    fi
    verified=1
    break
  fi
  parent="${current%/*}"
  [ "$parent" != "$current" ] && [ "$parent" != "$root" ] || break
  current="$parent"
  step=$((step + 1))
done
if [ "$verified" -ne 1 ]; then
  echo "Unable to verify USB VID:PID; refusing recovery" >&2
  exit 1
fi

interface="${iface##*/}"
case "$interface" in *[!A-Za-z0-9:._-]*|'')
  echo "Unsafe USB interface name" >&2; exit 1;;
esac
unbind="$xpad/unbind"; bind="$xpad/bind"
printf 'XPAD: event=%s VID:PID=%s:%s interface=%s driver=%s\n' \
  "$event" "$vendor" "$product" "$interface" "$xpad"
if [ "$mode" = "--dry-run" ]; then
  echo "DRY RUN: no device or driver changed"
  exit 0
fi
if [ "${MOONLIGHT_ALLOW_XPAD_REBIND:-}" != I_ACCEPT_DEVICE_DISCONNECT ]; then
  echo "Set MOONLIGHT_ALLOW_XPAD_REBIND=I_ACCEPT_DEVICE_DISCONNECT to apply" >&2
  exit 2
fi
if [ ! -w "$unbind" ] || [ ! -w "$bind" ]; then
  echo "xpad bind/unbind not writable; no changes" >&2
  exit 1
fi

unbound=0
restore() {
  if [ "$unbound" -eq 1 ]; then
    if printf '%s' "$interface" > "$bind"; then
      echo "XPAD: attempted rebind on cleanup" >&2
    else
      echo "XPAD: REBIND FAILED; reconnect controller or reboot test box" >&2
    fi
  fi
}
trap restore 0
trap 'exit 130' 2
trap 'exit 143' 15
trap 'exit 129' 1
printf '%s' "$interface" > "$unbind"
unbound=1
if printf '%s' "$interface" > "$bind"; then
  unbound=0
  echo "XPAD: rebind requested. Verify device enumeration."
else
  echo "XPAD: bind failed; cleanup will attempt restoration" >&2
  exit 1
fi
