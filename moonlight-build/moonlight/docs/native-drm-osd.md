# Native Amlogic statistics and DRM overlay reconstruction

This port is based on the native code paths observable in Vortex's distributed
`script.luna-2.7.1.277-f7dc33c8-NO-22.zip`, but is **not** a byte-equivalent
reconstruction or a demonstrated latency match.

## Verified native binary evidence

The original AArch64 Moonlight executable links `libdrm.so.2` and imports
`drmModeAddFB2`, `drmModeAtomicAlloc`, `drmModeAtomicAddProperty`,
`drmModeAtomicCommit`, `drmModeGetPlaneResources`, and the
`DRM_IOCTL_MODE_CREATE_DUMB` / `DRM_IOCTL_MODE_MAP_DUMB` ioctls. Strings
identify an ARGB8888 plane, Kodi DRM-fd borrowing, and a statistics panel:

- `FPS: %s dec / %s disp | DecDrop: %s | Queue: %s | Pool: %s`
- `Host: %s | RTT: %s | Assembly: %s | Decode: %s`

Other strings indicate network, CPU, temperature, and Amlogic queue/decoder
statistics. The main executable is stripped. These clues establish its
general approach, **not** a complete reconstructable source diff.

The kernel's framebuffer presentation still runs through the Amlogic
decoder/VFM, not through our OSD's DRM atomic commits. See the separate
`vfm-hevc-experiments.md` for decoder routing.

## Implemented code

- `src/osd/stats.c`: deterministic time-window sampling; native
  decoder submission count, V4L2 dequeues, kernel video-sink selected-frame
  changes per second (when available), dequeue-interval gaps, host
  processing latency, packet/FEC counts, RTT, and optional VFM queue/pool.
- `src/video/aml.c`: low-overhead opt-in counters from the actual
  Amlogic submit callback and V4L2 thread, exported as
  `aml_osd_snapshot` through the already-loaded native decoder module.
- `src/osd/osd.c`: an independent worker, Vortex-compatible
  `-osdargs` options and optional log fallback.
- `src/osd/drm.c`: two dumb ARGB8888 buffers, an *unused* overlay
  plane, atomic framebuffer updates, and release of owned resources.
- `src/config.c`, `src/main.c`: parse the `osd` and `osdargs`
  configuration that the imported Vortex Luna interface already writes;
  start/stop the worker around `LiStartConnection` / `LiStopConnection`.

The counter names are intentional: **Sub/s** is video decode-unit
submissions, **DQ/s** is successful V4L2 dequeues, **Sink/s** is
selected-frame changes in the kernel Amlogic video sink, and **Gaps25** counts
dequeue intervals longer than 25 ms. They do **not** directly measure
frames actually illuminated on the TV panel. Sink/s is closer to the frame
selected at v-sync, but does not confirm panel output. It is read-only from
`/sys/module/amvideo/parameters/display_frame_count` and shows n/a when the
counter is absent, invalid, reset or in its first sampling interval. The
counter is kernel-global, not automatically limited to one stream. The NG
kernel's `vframe_states` output includes `vframe buf_avail_num`, which
is now parsed with its full literal prefix. When direct VFM is used
and there is no V4L2 thread, DQ is shown as `n/a (direct)`, not zero.
Host time and assembly time are distinct from client decoder/display delay.
The first statistics sample is only a baseline and does not report FPS.

## Build and toggles

- Default upstream builds: `-DENABLE_DRM_OSD=OFF`. No new libdrm
  link dependency; the native sampler is compiled for the Amlogic target.
- Pannal CoreELEC 21 NG package overlay: `-DENABLE_DRM_OSD=ON`, adds
  the existing CoreELEC `libdrm` dependency.
- Luna already has an **OSD** toggle, which defaults OFF. Native
  `-osd` and `-osdargs` options also work with Moonlight CLI/config.
- `-osdargs "-font-size 20 -y 40 -interval 1.5 -x 20 -log"` produces
  an additional local logfile line each refresh. Without `-log` the
  sampler does not spam Kodi's log.

OSD is optional, and **must be OFF** for latency benchmarks; DRM
atomic commits may themselves affect rendering and presentation time.

## NG 4.9 kernel limitation

Vortex's CoreELEC 22 NO binary borrows a DRM descriptor from Kodi
via Linux `pidfd_open` (syscall **434**) and `pidfd_getfd` (syscall
**438**). This is confirmed by its AArch64 disassembly at
`0x40f308` and `0x40f324`, immediately before two calls to
`syscall@plt`. Pannal's CoreELEC 21 NG uses a Linux 4.9 lineage,
which predates `pidfd_getfd` (introduced in Linux 5.6). We do
**not** attempt to steal Kodi's graphics master.
The renderer opens `/dev/dri/card0` normally, requires DRM master,
atomic support, and an unused ARGB8888 **overlay** plane. If it cannot
obtain those, it reports that graphics output is unavailable, leaves the
video presentation pipeline unchanged, and supports text logging.

This is an explicit **feature-parity limitation**: the visual native
overlay may not work on NG without a different safe display mechanism
or kernel support. It is not a reason to modify CoreELEC firmware.
Do not assume that a successful cross-build proves DRM access.

## Verification and remaining work

Host CI tests check parser/metrics behavior, display-path labeling,
and compile-check the optional libdrm renderer. No physical DRM plane
or codec is exercised by those checks.

Device validation still needs to confirm:

1. Baseline Moonlight H.264/HEVC playback, no OSD or tuning.
2. OSD text-only diagnostic mode and expected counter samples.
3. Optional unused-plane discovery (if NG permits DRM master).
4. Exit and Sunshine disconnect do not leave a graphics plane allocated.
5. VFM-only and HEVC tuning-only frame-pacing comparisons.
6. Repeated high-speed-camera input-to-photon measurements with OSD
   and profiler **disabled**.

Vortex's full native decoded/displayed FPS, packet-loss estimation,
CPU/temperature panel and any remaining frame-pacing modifications are
**not fully reconstructed**. No end-to-end latency equivalence is claimed.

## Whole-device thermal/CPU diagnostic additions

The optional, low-frequency statistics worker now samples aggregate
`/proc/stat` CPU ticks and Linux `thermal_zone0/temp` in milli-Celsius.
The display says `SysCPU` (whole-system utilization) and `Th0`
(thermal zone zero); neither claims to measure Moonlight process CPU or
silicon core temperature. Unknown/invalid readings render as n/a.
There are no additional kernel writes. Do not enable OSD/profiling during
input-to-photon benchmarking.

## CoreELEC 21 decoder status improvements

Pannal's `media_modules-aml:amlogic-4.9.269`,
`drivers/frame_provider/decoder/utils/vdec.c`, exports the read-only
`/sys/class/vdec/vdec_status` class attribute. It reports per-decoder
`frame count`, `drop count`, `fra err count`, and `hw err count`.

The optional statistics worker now samples that interface, **matching the
currently streaming Moonlight codec and resolution** (H.264 or HEVC).
Multiple matching channels, missing fields, a reset, an unrecognized decoder,
or a missing kernel interface show **n/a**, not a fabricated zero or an
arbitrary other application's decoder statistics. The first sample establishes
a baseline; subsequent intervals compute `Dec/s` from increases in
`frame count` and per-interval decoder drop/error deltas.

These counters come from the *decoder driver*, not codec submission (Sub/s),
V4L2 dequeue (DQ/s), the video sink (Sink/s), or actual TV panel presentation.
They do not establish decode time in milliseconds. Hardware semantics may
vary between single-instance and multi-instance codec drivers.

## RTP packet rates, keyframes, and codec submission time

The optional OSD now collects `FRAME_TYPE_IDR` decode units and per-frame
mean **application-side codec submission** duration (assembly/copy/write).
The duration is **not** the hardware's decoding delay, a screen-present
timestamp, or input-to-photon latency. It remains separate from upstream
`frameHostProcessingLatency`.

RTP statistics use consecutive snapshots of
`LiGetRTPVideoStats()` to obtain video packets/s, FEC packets/s, FEC
recoveries and failures, out-of-sequence and invalid packets **per
measurement window**, with counter resets and impossible spikes suppressed.
`packetCountFecFailed` records **failed FEC recovery attempts** and
must not be labeled as total packet loss. No unreliable Mbps or
input-to-screen latency is inferred from these counters.

## Legible OSD panel on any available DRM overlay plane

The original two-line renderer clipped most diagnostic fields at the screen
right edge. `src/osd/layout.c` now wraps the output on word boundaries into
up to eight short rows (adjusted for output height), with a visible `...`
marker when the panel has insufficient space. Rows below the text are
transparent rather than shading the whole rectangle. The pixel renderer
still requires an unused ARGB8888 KMS overlay plane and DRM master; it
does not draw on Kodi's existing primary plane, write framebuffer memory,
or borrow its DRM descriptor.

This is a **layout fix only**: on NG Linux 4.9, graphical output remains
unverified and may fail entirely. The `-log` fallback always captures
full measurement strings independently of whether the panel is drawable.
