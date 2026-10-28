# Experimental CE21 NG FFmpeg/VFM pipeline

Source prototype, off by default. It has **not** been cross-built or run on an
AM6B+. No CoreELEC kernel/image/device-tree replacement is part of this design.
Do not describe this as verified zero-copy, latency parity, or HDR support.

## Configuration

Luna: Advanced settings → Video pipeline → **FFmpeg / VFM (experimental)**.
The default remains **Amlogic (standard)**. Missing prototype files, the wrong
kernel release, or an unpatched FFmpeg fail preflight before Kodi is stopped.
After driver setup starts, the existing launcher/watchdog restores Kodi on exit.

Native: `moonlight stream -platform aml-vfm …`, or set
`MOONLIGHT_VIDEO_PIPELINE=aml-vfm` when using automatic platform selection.
`moonlight check-vfm` only loads the userspace plugin and checks decoder features;
it does not touch the display or prove hardware compatibility. There is no silent
fallback to AML. Use `MOONLIGHT_FRAME_PACING=latency` (one pending frame) or
`balanced` (two); these bound the provider queue, not all decoder/network buffers.

The first prototype supports H.264/HEVC SDR, progressive linear NV12, even
dimensions up to 4096×2160 and requested rates up to 120. These bounds are **not**
claims that the hardware sustains every combination. HDR, AV1, rotation, dynamic
format changes and the native statistics overlay are excluded. FFmpeg capture
must export contiguous, identity-mapped DMA-BUFs with 32-byte-aligned strides;
unsupported layouts/modifiers are rejected instead of interpreted as linear.

## Pipeline and ownership

- A bounded four-packet input queue feeds a decoder worker. If input overflows,
  pending compressed packets are discarded and an IDR is requested; dependent
  packets are rejected until that IDR arrives.
- Private FFmpeg uses the stateful `h264_v4l2m2m`/`hevc_v4l2m2m` decoders and CE's
  DRM PRIME patch. A separate opt-in `nonblocking_dequeue` patch prevents the
  decoder worker from waiting indefinitely for a capture buffer. The worker polls
  for output at up to 2 ms intervals; that trade-off still needs measurement.
- `/dev/moonlight-vfm` imports the NV12 planes and registers a temporary named VFM
  route to the existing `amvideo` sink. It refuses an already active provider and
  never removes Kodi's existing VFM maps. No KMS/UVM/display-driver replacement.
- Frame references stay alive until the sink calls provider `put` and userspace
  consumes the completion. A frame observed by `peek` cannot be evicted between
  `peek` and `get`. Queue drops only retire frames not acquired by the sink.
- On close, including process death, the bridge stops publishing, requests
  a blackout policy, unregisters synchronously, removes only its own route, and then
  unmaps buffers. Reviewed CE21 source drains its display IRQ and disables the
  layer in this path; **this shutdown behavior must be verified on the actual
  installed kernel before normal use**. START passes the actual sysfs blackout
  policy to the bridge, which restores it after unregistering, even on a crash.
  A persistent policy during unregister avoids a race where the last display IRQ
  clears the transient FORCE_BLACKOUT flag before shutting down. After the IRQ
  drains, forced blackout protects the detached last frame until Kodi supplies
  a new frame, while the configured policy is restored.

`returned` counts buffer releases, not presentation timestamps or vblank events.
Decoder modules stay loaded between sessions; do not unload/reload them per game.

## Building without GitHub Actions

Use the existing native build setup locally. Set `MOONLIGHT_BUILD_VFM=yes` for a
clean Moonlight package build (`PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm`).
This enables `ENABLE_AML_VFM` and compiles `libmoonlight-aml-vfm.so`; the normal
native collector still makes the standard bundle. Copy the installed prototype
plugin separately into a staging `lib` directory. Changing the build flag on an
already cached CE package requires cleaning that package first.

1. Obtain pristine FFmpeg **8.1.2** and the reviewed Pannal CE source at
   `872edca4dd5a7c5d6344e785d7358766834d7119`.
2. Run `packaging/coreelec-ng/vfm/build-ffmpeg.sh FFMPEG_SOURCE COREELEC_SOURCE
   NEW_BUILD_DIRECTORY PRIVATE_PREFIX` with `VFM_CROSS_PREFIX` and `VFM_SYSROOT`
   pointing at the existing **ARM32** CE toolchain/sysroot. It copies the source,
   verifies CE's patch digest, applies both patches, and installs privately. Stage
   its `libavcodec.so.62` and `libavutil.so.60` with the prototype plugin.
3. Run `packaging/coreelec-ng/vfm/build-module.sh KERNEL_BUILD_TREE
   AARCH64_CROSS_PREFIX`. Kernel modules are ARM64 even though Moonlight is ARM32.
   The kernel tree must match the installed release, config and Module.symvers;
   `modules_prepare` alone cannot supply symbol CRCs. The script checks required
   exports and does not patch the kernel.
4. Supply compatible **4.9** stateful Amlogic V4L2 decoder modules if the device
   does not already have them. The prototype does not port or rebuild Skupi's
   5.15 binaries. Their complete build/export compatibility remains an unproven
   prerequisite. Include required modules in dependency order, bridge last.
5. Create the optional directory with `packaging/coreelec-ng/vfm/collect.py
   --libraries STAGED_LIB_DIRECTORY --kernel-release EXACT_RELEASE
   --module PATH_TO_DECODER_MODULE … --module PATH_TO_moonlight_vfm.ko
   --output NEW_VFM_DIRECTORY`. It validates architectures and vermagic and emits
   hashes. Skip decoder arguments only if those modules are already available.
6. In Luna, run `python3 packaging/package_ng.py STANDARD_NATIVE_BUNDLE
   --vfm-directory NEW_VFM_DIRECTORY --output /path/outside/checkout/luna-vfm.zip`.
   This remains a separate Kodi add-on. Existing nonprototype packages also work;
   selecting the experimental setting on one reports that its runtime is absent.

No workflow changes or dispatches are needed. A successful source check is not a
successful native build. FFmpeg libraries may be private; kernel modules still
need matching builds for each supported CE kernel.

## Required device gates

First verify decoder node discovery and its NV12 export capability. Then test a
short SDR stream, queued drops, decoder starvation, normal exit, forced process
death, repeated launch and Kodi playback afterward. Check for invalid DMA access,
provider leaks, stale display buffers and driver stalls. Stop this approach if
required functionality cannot be provided through the installed kernel's exports;
do not turn it into a maintained CoreELEC fork.
