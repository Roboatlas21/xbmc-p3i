# Safe OSD rendering on CoreELEC 21 NG / kernel 4.9

The first NG add-on must not modify Kodi's existing graphics ownership to
reproduce Vortex's statistics panel. This is a display-driver limitation,
not a reason to patch or flash CoreELEC.

## Current options

1. **Existing DRM renderer:** `src/osd/drm.c` opens
   `/dev/dri/card0`, requires DRM master and atomic mode support, and
   uses only an unused ARGB8888 overlay plane. It never borrows Kodi's
   descriptor. If Kodi holds the master while SIGSTOP'd, or the 4.9 Amlogic
   driver has no suitable atomic plane, the renderer fails and streaming
   continues. This path has only host syntax checks, no AM6B+ validation.
2. **Kernel framebuffer OSD planes (`/dev/fb0`/`/dev/fb1`):** The
   `linux-amlogic` 4.9 tree contains Amlogic framebuffer/OSD drivers under
   `drivers/amlogic/media/osd/osd_fb.c`. That proves an OSD mechanism exists
   but **not** that a separate plane is unused or safe for Moonlight to
   control. Kodi may retain framebuffer mappings when SIGSTOP'd. Direct
   writes and `FBIOPUT_VSCREENINFO` or `FBIOBLANK` can corrupt Kodi's GUI.
   **Do not automatically write to fb1.**
3. **Release/reacquire graphics ownership:** Fully stopping and restarting
   Kodi could release its display resources, but changes session startup
   time and may lose Kodi state; it is a separate launcher design requiring
   hardware validation. SIGSTOP by itself does not release file descriptors.
4. **Text-log fallback:** The current `-osdargs '-log'` behavior logs
   bounded-period statistics when graphical OSD is unavailable. It needs
   no graphics ownership and is the safe default for diagnosis.

Vortex's NO ELF uses `pidfd_open` and `pidfd_getfd` to access Kodi's DRM
descriptor. Kernel 4.9 does not normally provide `pidfd_getfd`. We must
not copy that approach or force a DRM master takeover.

## Read-only device probe

On a spare/test CoreELEC 21 NG device, execute:

```sh
sh /storage/.kodi/addons/script.luna/scripts/probe-coreelec-ng-osd.sh
```

That path is illustrative: copy `scripts/probe-coreelec-ng-osd.sh` from
this repository to a location on the device first; **the probe is not yet
bundled by the Luna ZIP packager**.

The script checks presence/readability of DRM and framebuffer devices,
current fb names/blank flags, the Amlogic sink frame counter, vframe stats,
and VFM mapping. It does not call DRM ioctls, attempt master acquisition,
write global sysfs, create OSD planes, or change Kodi's process state.

To progress safely, first obtain its output (with Kodi idle), then
re-check after launching Moonlight with **OSD off**. Independently establish
DRM master availability and available planes through a test binary using
read-only API calls. Do not implement a framebuffer fallback until both
plane ownership and restoration can be reliably proven on the AM6B+.

No graphical fallback has yet been demonstrated on NG.

## Read-only framebuffer ioctl inventory (additional)

A new `tools/probe-ng-fb.c` reads `FBIOGET_VSCREENINFO` and
`FBIOGET_FSCREENINFO` for the available fbdev nodes; it does not
write pixels or change settings. Its output complements the sysfs and
DRM plane probes. No independent fb1 overlay ownership has been established
for this CoreELEC 21 setup, so a graphical framebuffer fallback is not
enabled.
