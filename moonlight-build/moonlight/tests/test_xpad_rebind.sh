#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
script="$repo/scripts/rebind-xpad-device.sh"
sh -n "$script"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
root="$work/sys"
usb="$root/devices/platform/usb1/2-1"
iface="$usb/2-1:1.0"
inp="$iface/input/input12"
mkdir -p "$root/class/input/event4" "$root/bus/usb/drivers/xpad" "$inp"
printf '045e\n' > "$usb/idVendor"
printf '028e\n' > "$usb/idProduct"
touch "$root/bus/usb/drivers/xpad/unbind" "$root/bus/usb/drivers/xpad/bind"
ln -s "$inp" "$root/class/input/event4/device"
ln -s "$root/bus/usb/drivers/xpad" "$iface/driver"
export MOONLIGHT_XPAD_SYSROOT="$root" MOONLIGHT_XPAD_TEST_ONLY=1
sh "$script" /dev/input/event4 045e 028e > "$work/dry"
grep -Fq 'DRY RUN' "$work/dry"
[[ ! -s "$root/bus/usb/drivers/xpad/unbind" ]]
if sh "$script" /dev/input/event4 045e 028e --apply > /dev/null 2>&1; then
  echo "Unexpected apply without explicit confirmation" >&2; exit 1
fi
if sh "$script" /dev/input/event4 045e 9999 > /dev/null 2>&1; then
  echo "Unexpected success with wrong product ID" >&2; exit 1
fi
if sh "$script" /dev/input/event4 045e 028e-vendor > /dev/null 2>&1; then
  echo "Unexpected success with malformed VID/PID" >&2; exit 1
fi
export MOONLIGHT_ALLOW_XPAD_REBIND=I_ACCEPT_DEVICE_DISCONNECT
sh "$script" /dev/input/event4 045e 028e --apply > "$work/apply"
[[ "$(cat "$root/bus/usb/drivers/xpad/unbind")" == '2-1:1.0' ]]
[[ "$(cat "$root/bus/usb/drivers/xpad/bind")" == '2-1:1.0' ]]
rm "$iface/driver"
ln -s "$root/bus/usb/drivers/hid-generic" "$iface/driver"
mkdir -p "$root/bus/usb/drivers/hid-generic"
if sh "$script" /dev/input/event4 045e 028e > /dev/null 2>&1; then
  echo "Unexpected fallback from non-xpad driver" >&2; exit 1
fi
echo "Guarded xpad dry-run, identity check, and test-only rebind: PASS"
