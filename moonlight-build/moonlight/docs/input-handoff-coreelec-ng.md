# CoreELEC NG input-device handoff

Vortex's stripped NO executable has an additional HID/xpad unbind/rebind and
USB driver ID registration path. These are kernel-global actions and cannot
be copied safely without identifying affected hardware and an exact rollback.

Current source implementation intentionally separates two safer pieces:

- `MOONLIGHT_INPUT_HANDOFF_PROBE=1` enables a **read-only** bounded scan
  of `/proc/[pid]/fd` for other processes with `/dev/input/eventN` open.
  Selected Luna input devices are filtered by exact paths; without an
  explicit selection, all event devices are considered. Output includes
  `INPUT_HANDOFF_V1`. An open descriptor is **not proof of exclusive
  EVIOCGRAB**; inaccessible processes cannot be inspected.
- `MOONLIGHT_INPUT_RECOVERY=1` enables 3 retries of a *failed* event-device
  open, 150 ms apart, only for `EBUSY`, `EAGAIN`, or `EACCES`. This can
  recover a transient release and does not affect the normal fast path.
  It never performs a driver unbind, `new_id` write, EVIOCGRAB probe,
  or intervention in Kodi. A permanent hold still fails.

Both variables default OFF; neither setting is enabled for normal Kodi use.
These two behaviors are **not full Vortex HID/xpad rebind parity**.
The next stage requires real AM6B+ device evidence including USB IDs,
current driver ownership, whether a device is truly inaccessible, and
the effect of Kodi's paused file descriptors. Do not use blind sysfs
unbind/rebind: it can disconnect a remote or controller and survive until
manual recovery after a crash.

The test focuses on exact event paths, opt-in switches, retry limits and
common failures. It is not a substitute for a controller unplug/replug or
Kodi-to-Moonlight transition test.
