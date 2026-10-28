# Vortex-to-NG native frame and input audit (2026-10-25)

Target: Ugoos AM6B+, Pannal CoreELEC 21 NG ARM32.
Reference: Vortex's stripped CoreELEC 22 NO ARM64 Luna ZIP
`script.luna-2.7.1.277-f7dc33c8-NO-22.zip`, SHA-256
`74c93039401b146b87fb67b62f95f1054834d48fba0bb5d88c64c639b78d0837`.

This document distinguishes a **binary observation**, a **source comparison**,
and a **hypothesis**. Vortex's original C sources are not available. Our
`reference/vortex-no22` branch contains notes, not a byte-equivalent source
reconstruction. Re-check the original ELF before attributing an algorithm
to Vortex; do not treat a release-note claim as a measured latency improvement.

## Frame-presentation evidence

| Area | Confirmed binary observation | Current NG fork | Conclusion |
| --- | --- | --- | --- |
| PTS check-in | Vortex divides `presentationTimeUs` by 1000 before `codec_checkin_pts` | Current NG uses `codec_checkin_pts_us64()` with microsecond timestamps in `src/video/aml.c` | Intentional difference; not matched at code level |
| Amlogic frame input | Vortex concatenates frame buffers and calls `codec_write` | Same approach; five EAGAIN retries of 2ms | No Vortex-only retry algorithm established |
| Freerun | `codec_set_freerun_mode(1)` present | Present already in upstream | Not novel |
| V4L2 display thread | `VIDIOC_DQBUF`/`VIDIOC_QBUF`, with 500us EAGAIN retry | NG supports latency and balanced pacing; balanced uses a bounded dequeued-buffer ring and retains indices when QBUF fails | Current behavior differs from the original upstream pump |
| VFM routing | Direct `vdec.h264.00`/`vdec.h265.00` routes were observed | Explicit Latency/Balanced `video_path`; no global VFM writes | Legacy override removed |
| HEVC decoder policy | Two module parameters modified, original values restored | Automatic for HEVC when values match known NG defaults; restored on cleanup | Matches Skupi policy in source; hardware result unknown |
| Native DRM overlay | Vortex uses DRM atomic plane APIs and borrows Kodi's fd using pidfd syscalls | Own-fd unused-plane renderer, no pidfd | Not graphical parity on NG |
| Actual video output rate | Original overlay advertises display FPS and decoder statistics | Previous NG version counted submissions/dequeues only | Need evidence-based sink counter |
| Input lag changes | Vortex release note says reduced input lag | `src/input/evdev.c`, `udev.c`, `mapping.c` are unchanged from upstream `f7dc33c` | Native input-optimization parity **unverified** |

The last input row is a source-diff finding. Comparing
`f7dc33c87064e97f7f4e29457e312b9cceb24349` with
`feat/coreelec-ng` shows no changes to the core evdev/udev mapping
implementation. That is not proof that Vortex changed it: performance
could have improved from kernel or frame-path changes elsewhere.

## Verified new NG video-sink counter

Pannal `linux-amlogic:amlogic-4.9-20`,
`drivers/amlogic/media/video_sink/video.c`:
- `display_frame_count` is incremented at a selected-frame change inside
  the video sink's vertical-sync handling when
  `cur_dispbuf_back != cur_dispbuf` (around source line 6075).
- `module_param(display_frame_count, int, 0664)` exposes the value as
  `/sys/module/amvideo/parameters/display_frame_count` when the built-in
  driver exports module-parameter sysfs attributes.
- The same driver exposes
  `/sys/class/video/vframe_states` and prints
  `vframe_pool_size=...` and **`vframe buf_avail_num=...`**.
  The latter has a `vframe ` prefix and cannot be parsed with the
  old bare key `buf_avail_num`.

The video-sink counter is closer to **selected frame changes**, but remains
system-wide kernel telemetry, not proof of a displayed pixel at the TV.
It can reset across sessions, stay flat in a different routing path, or be
absent on a modified device. Report missing counters as n/a. Never infer
photons-on-screen or true input-to-photon latency from these values.

## Main executable audit completed

The original Vortex release ZIP was recovered and its **main AArch64 ELF**
freshly disassembled. See [vortex-main-elf-audit.md](vortex-main-elf-audit.md)
for instructions, addresses, a detailed input/ALSA analysis, and the verified
CEC volume-routing discrepancy.

Additional findings: a missing native HID/xpad device handoff/rebind routine,
an ALSA stale-buffer resynchronization path, and **no confirmed new**
main-executable video presentation scheduler. Do not conflate these with
measurements of video frame latency.

## Remaining evidence needed

1. Diff native frame callbacks against released Vortex source if published.
2. On NG device, determine VFM route in baseline and tuned mode, verify sink
   counters and check for unintended changes after exiting.
3. Repeatable 1080p60 H.264/HEVC benchmark under the same OS image, with OSD
   and decoder profilers disabled. Measure input-to-photon with a camera.
4. Reverse-diff Vortex's `libmoonlight-common.so.4` separately if protocol
   timing or packet-queue differences are suspected.

Not a confirmed Vortex-specific optimization: freerun, five EAGAIN retries,
the original 500us V4L2 polling, or ARM64 bitness alone.
