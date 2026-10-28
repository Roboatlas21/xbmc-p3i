# Vortex native decoder and frame-timing investigation

This documents the evidence from Vortex's provided
`script.luna-2.7.1.277-f7dc33c8-NO-22.zip`, and defines reproducible
measurements. The binary is CoreELEC 22 NO / AArch64; our target is
Pannal CoreELEC 21 NG / ARM32.

## Confirmed in the AArch64 library

Disassembly of `lib/libmoonlight-aml.so` resolves the public functions
`aml_submit_decode_unit` (0x1650), `aml_setup` (0x18a0),
`aml_cleanup` (0x1d10), and `aml_display_thread` (0x13b0).

- **Presentation timestamps:** the native instruction sequence at
  0x16b4–0x16e0 divides the microsecond timestamp at decode-unit offset
  0x20 by 1000, before `codec_checkin_pts`. This corresponds to
  `decodeUnit->presentationTimeUs / 1000` in the pinned
  `moonlight-common-c` header. The older upstream C source incorrectly
  called this field `presentationTimeMs`. We corrected it.
- **Decode submission:** a frame's buffer list is concatenated, passed to
  `codec_write`, and returned as `DR_NEED_IDR` on remaining bytes.
  The compiled path retains five EAGAIN retries with 2000-us sleep
  (0x1758–0x176c), matching upstream's retry budget.
- **Display polling:** `VIDIOC_DQBUF` / `VIDIOC_QBUF` are used, with
  500-us sleep on EAGAIN (0x13b0–0x1490). These primitives are already
  present in the upstream Amlogic renderer.
- **Freerun:** `codec_set_freerun_mode(1)` is called; upstream already does
  this. Calling it Vortex-specific would be inaccurate.
- **Hardware pipeline:** Vortex rewrites the VFM default using
  `rm default` and `add default %s amvideo` where the source is
  `vdec.h264.00` or `vdec.h265.00`, then restores `decoder amvideo`.
  Our port now contains an **opt-in**, guarded implementation in
  `src/video/aml_tuning.c`, with the normal path unchanged.
- **HEVC sysfs:** the binary references
  `/sys/module/amvdec_h265/parameters/nal_skip_policy` and
  `ref_frame_mark_flag`, retaining original values for restoration.
  Pannal's NG driver exposes both settings; the source default is
  `nal_skip_policy=2` and nine `ref_frame_mark_flag=1` entries.
  The reconstructed opt-in path writes `0` and nine zeros respectively,
  then attempts restoration of the saved values at cleanup.
- **Reference invalidation:** `MOONLIGHT_HEVC_RFI=0` disables HEVC RFI;
  our fork has the corresponding opt-in toggle. This is a compatibility
  fallback, not a measured speedup.

The main Vortex Moonlight executable also contains a DRM-plane statistics
overlay showing decoded/displayed FPS, drops, queue/pool, RTT, assembly and
decode statistics. Those features are **not** reconstructed by this profile.

**Important:** The unchanged retry budget and freerun call mean we have
not established any novel low-latency algorithm in the decoder's submit loop.
Any measured advantage might instead arise from VFM setup, kernel drivers,
the rest of Moonlight, audio processing, or display pacing.

## Our optional diagnostics

Set `MOONLIGHT_AML_PROFILE=1` in the Moonlight process environment,
stream as normal, and exit to Kodi. The program emits one line, prefixed
`AML_PROFILE_V1`, into Luna's logfile.

The sample includes:

- `assembly_mean_us`: receive-to-assembly interval reported by
  moonlight-common-c, **not** local video decode latency.
- `submit_mean_us`, `submit_max_us` and
  `submit_p95_bucket_us`: elapsed clock time within the Amlogic decoder's
  submission callback. The p95 is an **upper bound**, bucketed to a fixed
  threshold; zero denotes an overflow above 132000 us.
- `write_mean_us`: wall time around `codec_write` and EAGAIN waits,
  not GPU/hardware decoding time.
- `eagain`, `write_calls`, `failed`: flow control and errors.
- `v4l2_dqbuf_cycles`, `v4l2_poll_eagain`: V4L2 operations on the
  renderer thread, **not** counts of actually displayed video frames.

No per-frame logging or network reporting is performed. The module is
disabled by default. Logging a single summary at cleanup helps keep runtime
measurement overhead small, but even timestamp calls can affect results.

Compare the logs from two versions of **our instrumented build** with:

```sh
python3 scripts/compare_aml_profiles.py /path/to/baseline.log /path/to/candidate.log
```

Vortex's closed AArch64 binary does **not** emit `AML_PROFILE_V1`.
Its OSD values and our profiling values are not interchangeable. Do not
treat the output as a head-to-head decoder or latency result.

## Proper end-to-end comparison on the AM6B+

1. Use the same Sunshine host, 1920x1080/60 H.264 profile, bitrate,
   controller/mouse/keyboard, wired network, television input and Game Mode.
2. Keep one reference NO installation with Vortex's build and one NG
   installation with our build. Record their exact CoreELEC/Kodi and decoder
   versions: this is not a controlled one-variable comparison.
3. In each session use identical host desktop actions. Record startup time,
   drops, audio failures and Moonlight/Vortex stats, if available.
4. Measure **input-to-photon latency** with a high-speed camera showing the
   button or mouse click and the television response in the same shot.
   Gather many samples, compute median and p95, and repeat across sessions.
5. Run the camera test with our profiler **disabled**, then separately
   enable profiling to investigate any frame-submission stalls.
6. For eventual decoder-only comparisons, benchmark two source revisions
   against the **same NG kernel/toolchain**, with identical conditions.
   An NO-vs-NG camera test also captures different kernel/driver behavior.

**Nothing in the current source-level or host CI tests establishes that
Vortex's original low-latency performance has been reproduced.** Do not
call the port feature-complete until on-device data justifies it.
