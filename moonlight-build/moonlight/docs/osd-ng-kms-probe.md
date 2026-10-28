# CoreELEC 21 native on-screen display capability decision

Vortex's CoreELEC 22 NO OSD uses DRM atomic overlay planes and borrows
Kodi's DRM FD using Linux 5.6+ `pidfd_getfd`. Pannal's NG Linux 4.9
kernel lacks that normally. When Kodi is suspended with SIGSTOP its open
DRM descriptors and KMS master status are **not released**.

Existing `src/osd/drm.c` attempts its own, unused ARGB8888 overlay
plane only when visual OSD is explicitly enabled. It refuses to modify
any in-use plane and falls back to `-osdargs '-log'` if an independent
DRM master or atomic plane is unavailable. No on-device proof exists.

## New non-invasive capability probe

`tools/probe-ng-drm.c` lists available KMS CRTCs and DRM planes using
read-only GET ioctls and a per-FD `drmSetClientCap`. **It never calls
`drmSetMaster`, `drmModeAtomicCommit`, framebuffer creation, a VFM
write, or a USB driver write.** It can identify an unused ARGB8888 overlay
candidate but does not establish that it's usable or safe to take.

Example on an environment with libdrm development files:

```sh
cc tools/probe-ng-drm.c -o probe-ng-drm $(pkg-config --cflags --libs libdrm)
./probe-ng-drm /dev/dri/card0
```

A CoreELEC device usually has no compiler, so cross-compile against its
own sysroot before running it. The existing
`scripts/probe-coreelec-ng-osd.sh` remains a zero-compile sysfs
inspection option. Its output can be collected on a test box.

## Choice

Until actual AM6B+ probes establish that a **separate** plane and lawful
master access are available, the safe option for CoreELEC NG is **text
statistics logging only**. Do not unblank/reconfigure `/dev/fb0` or
`/dev/fb1` under paused Kodi; the Amlogic framebuffers remain in use.
Do not use `pidfd_getfd` on kernel 4.9 or backport it merely for a
diagnostic OSD.

This preserves playback stability at the expense of Vortex visual
statistics parity. It does not mean Moonlight video presentation needs
DRM atomic mode; the Amlogic decoder/VFM is a separate path.

## Additional CoreELEC 21 fbdev probe

`tools/probe-ng-fb.c` inspects `/dev/fb0`, `/dev/fb1`, and `/dev/fb2`
using `O_RDONLY`, `FBIOGET_FSCREENINFO` and `FBIOGET_VSCREENINFO`.
It reports framebuffer identity, geometry, advertised color transparency,
and memory size. It performs **no mmap, ioctl to set screen info, blanking,
modesetting, framebuffer writes, plane commits, or DRM master request**.

To compile on an NG-compatible Linux toolchain:

```sh
cc tools/probe-ng-fb.c -o probe-ng-fb
./probe-ng-fb
```

The reported `transp` channel and existence of `/dev/fb1` are
**necessary but insufficient** evidence for a separate independent overlay:
fb1 can be reserved by Kodi, may alias the primary compositor, or have
vendor alpha behavior that cannot be inferred from the mode flags.
Do not change fbdev hardware state until the exact AM6B+ kernel and Kodi
process handoff have been validated.

**Renderer feasibility decision:**
- If an unused atomic DRM overlay and exclusive DRM master are confirmed
  on the test device, the existing DRM renderer can be tested.
- If the only path is fb1, identify device ownership and obtain an
  explicit, reversible lease/hand-off from Kodi **before** implementing
  a framebuffer writer. Linux fbdev doesn't automatically provide such
  a lease; detecting no open fd is not ownership proof.
- Otherwise, keep Vortex-style statistics in a periodic text log and
  disable graphical OSD. There is no demonstrated safe graphical backend
  for Linux 4.9 NG yet.
