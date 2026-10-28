# Manual recovery of a specifically verified Xbox/xpad controller

Vortex's NO executable contains HID/xpad rebind and driver-ID registration.
Automatically performing this on CoreELEC NG would risk globally disconnecting
controllers, remotes, or Kodi's own input handling. The **default Luna
launch path performs no sysfs writes**.

For advanced testing only, `scripts/rebind-xpad-device.sh` provides an
explicit *per-device* recovery tool. Default behavior is a **dry run**.
It verifies an `/dev/input/eventN` path, an exact four-digit USB vendor
and product ID, and that the event device's ancestor USB interface is
bound to the **xpad** driver. Other drivers, including hid-generic, are
rejected. It never writes `new_id`, reboots, or rebinds arbitrary HID
devices.

Dry run on a disposable/test CoreELEC box (replace with actual VID/PID):

```sh
sh rebind-xpad-device.sh /dev/input/event4 045e 028e
```

Only if the output matches the intended controller and you have keyboard/
SSH access for recovery, an explicit, **manual** `--apply` mode exists:

```sh
MOONLIGHT_ALLOW_XPAD_REBIND=I_ACCEPT_DEVICE_DISCONNECT \
  sh rebind-xpad-device.sh /dev/input/event4 045e 028e --apply
```

It writes the single validated USB interface ID to xpad's `unbind`
and then `bind`, with a cleanup trap that retries binding on catchable
errors. **SIGKILL, kernel failure, device disappearance or power loss can
still interrupt restoration.** The script is intentionally not invoked by
Moonlight or installed by the Luna package. It is *not* validated on an AM6B+.
The safe integrated option remains only read-only reporting and short retries.

Before using --apply, confirm the device truly is inaccessible, capture
its actual sysfs ancestry, verify Kodi's remote remains operable, and prepare
to reboot the test CoreELEC device if rebinding fails.

The script uses POSIX `/bin/sh` and BusyBox-compatible utilities; no Bash
installation is required on CoreELEC NG. It still requires explicit manual
copying to a test device and root privileges for `--apply`.
