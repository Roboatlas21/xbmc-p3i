# Experimental CE21 NG FFmpeg/VFM pipeline

Experimental pipeline, off by default. The native workflow can now build and
package its complete runtime: private ARM32 FFmpeg/plugin, stock CE21 decoder
modules and the ARM64 VFM bridge, inside an installable Luna ZIP. No CoreELEC
kernel/image/device-tree replacement is shipped. Device streaming, teardown,
zero-copy behavior and latency remain unverified; HDR is unsupported.

The first device test reached FFmpeg but could not find a decoder: CoreELEC's
`coreelec_common.dtsi` disables `vcodec_dec`, overriding the enabled G12B base
node. Bridge version 2 registers a runtime `aml-vcodec-dec` platform device when
that known node is disabled and no decoder device already exists. It binds the
already loaded stock `amvdec_ports` driver; it does not edit the device tree.
An existing bound decoder is reused, and an unbound device fails explicitly.
Luna then queries the real device's capabilities and H.264/HEVC/NV12 formats
before starting Moonlight. This fixes the missing activation step; successful
decoding and presentation still require another test on the box.

After updating from bridge version 1, reboot CoreELEC once if VFM was used since
boot. Luna detects the older loaded bridge before stopping Kodi and asks for
that reboot. It never forces a decoder or bridge module to unload.

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
When a module was already loaded at boot, native VFM setup enables
`amvdec_ports/parameters/multiplanar` before opening a decoder. It stays enabled
between sessions, matching Luna's `multiplanar=1` option for newly loaded modules.

## Complete build and installable addon

In **this Moonlight repository**, run **CoreELEC 21 NG ARM32 native build** on
`feat/coreelec-ng` with **Build complete experimental VFM runtime and installable
Luna addon** enabled (`build_vfm_runtime=true`). The standard build remains the
default when this input is off. This is separate from the older snapshot
workflow's `build_vfm_plugin` compile-check input.

The workflow:

1. Pins Pannal CoreELEC, its 4.9 kernel and media modules to the reviewed revisions
   in `packaging/coreelec-ng/vfm/sources.json`.
2. Builds private FFmpeg 8.1.2 with the checked CE DRM PRIME patch and our
   nonblocking dequeue patch. Both the plugin and runtime use these libraries.
3. Builds the stock kernel to obtain its exact configuration, exported symbol
   CRCs and V4L2 support modules; builds the stock media modules and our bridge
   against that output. The kernel image is a build dependency only.
4. Collects `amvdec_ports`, `amvdec_mh264`, `amvdec_h265`, their recursive module
   dependencies, and `moonlight_vfm` last. These CE21 codecs support V4L2 without
   Skupi's 5.15 `*_v4l` modules. The bridge activates the stock decoder device at
   runtime when the CoreELEC device tree leaves it disabled. Nothing patches an
   installed kernel or device tree.
5. Checks ARM32 libraries, ARM64 modules, vermagic, library dependencies and
   relocatable paths; records hashes and kernel/source provenance. Runs the
   **actual ARM32 plugin** under QEMU to verify its decoder capabilities.
6. Uses the existing pinned public Luna source export, read-only, to package
   native Moonlight plus the complete VFM runtime and license notices. This
   avoids another private-repository token and does not update the snapshot.

Download the `luna-coreelec21-ng-vfm-addon` artifact. Install its
**`luna-coreelec21-ng-vfm.zip`** using Kodi's *Install from ZIP file*, then select
**Advanced settings → Video pipeline → FFmpeg / VFM (experimental)**.
The artifact also contains the reusable `moonlight-coreelec21-ng-vfm-runtime.tar.gz`
and manifest. The standard native tar alone does not include VFM.

Luna validates the bundle before stopping Kodi and loads the missing modules
when launching Moonlight. Modules already loaded remain in place. The exact
kernel release must match; the kernel also enforces symbol CRC compatibility.
Do not bypass a mismatch with forced module loading. A compatible bundle must
be rebuilt for a different kernel; a successful QEMU probe does not prove
installed-kernel or device compatibility.

## Local build

Use the same pinned checkouts and CoreELEC build prerequisites as the native
workflow. Keep the kernel at `linux-amlogic-local` beside `CoreELEC`, symlinked
from `$HOME/linux-amlogic-local`, and media modules at
`CoreELEC/sources/media_modules-aml/media_modules-aml-local`.
Set `PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm MOONLIGHT_BUILD_VFM=yes` in the
build environment. From the Moonlight source root:

```sh
bash packaging/coreelec-ng/prepare-build.sh /path/to/CoreELEC
bash packaging/coreelec-ng/build-stage.sh toolchain /path/to/CoreELEC /path/to/logs
bash packaging/coreelec-ng/build-stage.sh deps-first /path/to/CoreELEC /path/to/logs
bash packaging/coreelec-ng/build-stage.sh deps-final /path/to/CoreELEC /path/to/logs
bash packaging/coreelec-ng/build-stage.sh vfm /path/to/CoreELEC /path/to/logs
(cd /path/to/CoreELEC && ./scripts/clean moonlight-embedded)
bash packaging/coreelec-ng/build-stage.sh moonlight /path/to/CoreELEC /path/to/logs
bash packaging/coreelec-ng/collect-native.sh /path/to/CoreELEC /path/to/native
python3 packaging/coreelec-ng/vfm/collect-runtime.py /path/to/CoreELEC /path/to/new-vfm
bash packaging/coreelec-ng/vfm/smoke-runtime.sh /path/to/CoreELEC /path/to/new-vfm
python3 packaging/coreelec-ng/vfm/package-addon.py /path/to/pinned-export/moonlight-build \
  /path/to/native/moonlight-coreelec21-ng-arm32.tar.gz /path/to/new-vfm /path/to/luna-vfm.zip
```

Use new collector output directories. `modinfo`, `readelf`, and `qemu-arm` are
required. Cleaning Moonlight before rebuilding prevents cached AML-only output
from surviving a change to the VFM build flag. The lower-level `build-ffmpeg.sh`,
`build-module.sh` and `collect.py` remain available for manual debugging.
`modules_prepare` alone cannot supply the symbol CRCs needed for shippable modules.

## Validation status

The public workflow has compiled ARM32 Moonlight and all VFM dependencies and
collected 17 kernel modules and four private libraries. A separate local ARM32
build of patched FFmpeg, libdrm and the plugin passes both decoder feature checks
under QEMU using CoreELEC's `/usr/lib` loader layout. The smoke script invokes
that loader explicitly because the compiler sysroot lacks the OS image's
`/lib` symlink. Missing loaders or bundled FFmpeg libraries still fail the test.
Packaging tests
exercise dependency ordering and reject missing libraries/modules, mixed
architectures, mismatched kernel releases and missing symbol-version metadata.
The complete CoreELEC workflow, including QEMU against its exact binaries, and
actual AM6B+ streaming remain unverified.

## Required device gates

First verify decoder node discovery and its NV12 export capability. Then test a
short SDR stream, queued drops, decoder starvation, normal exit, forced process
death, repeated launch and Kodi playback afterward. Check for invalid DMA access,
provider leaks, stale display buffers and driver stalls. Stop this approach if
required functionality cannot be provided through the installed kernel's exports;
do not turn it into a maintained CoreELEC fork.
