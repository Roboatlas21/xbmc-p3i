# Dolby Vision L5 follows the displayed picture

Zooming a native Dolby Vision movie moves its pixels through VPP, but P3i
forwards the effective L5 active-area offsets unchanged. With a letterboxed
3840x2160 source, the TV can consequently treat strips of the zoomed picture
as outside the active area.

This directory carries the **kernel-side fix** on the `xbmc-p3i` fork. Replacing
Kodi or the zoom add-on alone does not install it. The patch targets
[`pannal/linux-amlogic` at `1f2292a39cec1e881d18ff77614cc663bc2dd760`](https://github.com/pannal/linux-amlogic/commit/1f2292a39cec1e881d18ff77614cc663bc2dd760),
the inspected P3i development kernel. It does not change the proprietary Dolby
libraries, the RPU, Kodi's source metadata, or the add-on's zoom logic.

## Implementation

`video.c` supplies the displayed frame's actual VPP source crop and destination
rectangle. The transform intersects each L5 active interval with that crop,
maps its edges to output coordinates, then computes output edge offsets.
Inclusive register endpoints become half-open pixel-edge intervals; rounding
expands the active area by less than one output pixel to avoid excluding picture
pixels. A 2160-line source with top/bottom 276/277 and a visible source interval
of `[140,2020)` produces 156/157 output offsets at 2160 lines.

The driver transforms a separate Core3 metadata copy **after** existing source,
detector, override, OSD and subtitle selection. It leaves all-zero suppression,
other metadata levels and packed register padding intact. Retained metadata is
never rewritten, so subsequent frames, scene changes and zoom reset cannot
compound a previous transformation. The geometry enters under the existing DV
transaction guard; a busy transaction leaves it pending for the next vsync.
Geometry-only changes request a Core3 update even while playback is paused,
without requesting another metadata parse or changing the video frame.

Supported output is progressive, linearly scaled native DV from a **16:9 coded
raster** (including letterboxed/pillarboxed content within that raster). Both
legacy and newer TV-led backends share the corrected Core3 path. HDR/SDR,
low-latency DV, certification mode, interlace, rotation/mirroring, nonlinear
stretch, 3D, and uncompressed double-write surfaces with different dimensions
retain their existing behavior. Cropped encodes with non-16:9 coded dimensions
also retain their existing behavior: P3i can author their L5 against a synthetic
canvas, whose provenance is not available in this kernel interface. Guessing
that reference canvas would introduce a separate regression.

## Validate and apply

Requires Git, Python 3 and a C compiler. These commands do not start CI or install
anything on a playback device.

```sh
# From the xbmc-p3i checkout:
python3 tools/coreelec/l5-transform/tests/run_tests.py

git clone -c core.autocrlf=false --no-checkout https://github.com/pannal/linux-amlogic.git linux-l5
git -C linux-l5 checkout --detach 1f2292a39cec1e881d18ff77614cc663bc2dd760
python3 tools/coreelec/l5-transform/apply_kernel.py linux-l5 --check
python3 tools/coreelec/l5-transform/apply_kernel.py linux-l5
```

`apply_kernel.py` checks the hashes of every original file and verifies the full
patch before writing anything. It refuses a different source revision or a
second application. `--git /path/to/git` selects a non-default executable.

For a CoreELEC image build, use that pinned kernel revision in the existing P3i
build configuration and copy `linux-990-dv-l5-display-geometry.patch` into:

```text
projects/Amlogic-ce/devices/Amlogic-ng/patches/linux/
```

Use an **unpatched** kernel source when letting CoreELEC apply the patch. Clean
the Linux package before rebuilding to invalidate a previously built kernel.
Build the complete image with its matching modules using the existing T5 build
configuration. Do not combine a replacement Kodi binary with the old kernel and
expect this fix to be present. The Kodi fork's unrelated Moonlight workflow is
not changed or dispatched by this branch.

## Verification

The test runner extracts the actual helper headers from the patch, rather than
testing a separate implementation. It checks identity, approximately 115% crop,
asymmetric positioning, zoom-out, resolution conversion, simultaneous horizontal
and vertical borders, scene changes, no compounding, original-metadata integrity,
zero suppression, malformed/truncated/duplicate blocks, invalid geometry and
40,000 deterministic randomized metadata/geometry cases. Assertions remain
enabled even when the compiler defines `NDEBUG`.

```sh
python3 tools/coreelec/l5-transform/tests/run_tests.py --cc clang \
  --cflags=-fsanitize=address,undefined
python3 tools/coreelec/l5-transform/tests/run_tests.py \
  --cc 'zig cc -target aarch64-linux-gnu' --compile-only
```

Validated during implementation: native tests with warnings as errors,
undefined-behavior instrumentation, AArch64 helper cross-compilation, and clean
patch application to the pinned source. A complete CoreELEC image build and
AM6B+/TV visual verification are still required; host tests do not establish
hardware behavior or whole-kernel compilation.

On the rebuilt image, `l5_transform` defaults to enabled. For a direct comparison
on the same paused scene and zoom setting:

```sh
cat /sys/module/amdolby_vision/parameters/l5_transform
echo 0 > /sys/module/amdolby_vision/parameters/l5_transform
echo 1 > /sys/module/amdolby_vision/parameters/l5_transform
```

After closing the OSD, check 100%, 115%, reset, vertical shift, and a scene with
different bars; repeat while paused. Check OSD/subtitle appearance and dismissal,
seek, stop/restart, and the next movie. The transformed boundaries should track
the picture while source L5 reported to the add-on remains unchanged. Confirm
both TV-led backends separately. A photo of the TV is needed to assess these
sink-side strips; a Kodi screenshot alone may not capture them.
