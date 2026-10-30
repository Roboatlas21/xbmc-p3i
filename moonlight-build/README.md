# Luna NG public build snapshot

This directory exports Moonlight and Luna source files without importing their
development histories. The initial snapshot's parent belongs to this public
Kodi fork; its existing upstream history remains intact. Exact source and
dependency revisions are recorded in `source-lock.json`. Source licenses and
assets are retained. Original workflow files remain inside the exported source
subdirectories and are not active workflows in this repository.

## Manual build

Open **Actions → Luna NG public snapshot build → Run workflow**. Leave the
workflow branch on `aml-4.9-21.3` and `snapshot_ref` on `build/luna-ng-snapshot`, or
enter a specific snapshot commit. Preparation resolves that ref once; the native
job checks out the same immutable commit. No push, pull-request or scheduled
trigger starts this workflow.

The workflow is also registered on the default branch so GitHub displays the
manual button. Only that workflow file is added there; these source snapshots
live on the dedicated build branch. No replacement Kodi, CoreELEC image, kernel
or device tree is shipped or installed. No private repository token is needed.

The default build produces the standard ARM32 Amlogic native bundle and an
installable `luna-coreelec21-ng.zip` in the **luna-coreelec21-ng-addon** artifact.
Standard Amlogic remains selected by default. A standard-only build reports
that VFM is unavailable if its experimental setting is selected.

For VFM, check **Build complete experimental VFM runtime and installable Luna
addon** (`build_vfm_runtime=true`). This replaces the former compile-check input.
The **luna-coreelec21-ng-vfm-addon** artifact then contains:

- **`luna-coreelec21-ng-vfm.zip`**, ready for Kodi's *Install from ZIP file*.
- Private patched ARM32 FFmpeg, the VFM plugin, ARM64 decoder dependencies and
  the VFM bridge, all included inside that ZIP.
- A reusable VFM runtime tar, kernel/module manifest, source lock and checksums.

After installation, select **Advanced settings → Video pipeline → FFmpeg / VFM
(experimental)**. Luna loads missing modules when Moonlight launches; the native
backend also configures a decoder module that was already loaded at boot.
The installed kernel must be compatible with the bundled modules. Module release
and symbol-version checks remain enforced.

The updated VFM bridge activates the stock decoder device at runtime when
CoreELEC's device tree disables it. Version 4 imports decoded buffers through
the existing amvideo display device's DMA configuration, addressing the first-frame
mapping error from version 2, and uses the existing per-frame freerun flag to
bypass media-clock timestamp gating. This flag is separate from TV Game Mode.
Version 5 additionally preserves the linear-frame flag required by CE21's
display interface for V4L2 capture buffers. Packaging and launch checks require
version 5 and reject older bridge versions. The version 5 change has local regression coverage; its native module build
and device picture-quality test are still pending. After replacing a
previously loaded older bridge, reboot CoreELEC once; Luna detects this case
before stopping Kodi. The launcher and watchdog now also recognize CE21's
decorated tsync status when preserving and restoring that setting.

**Advanced settings → Automatically request TV Game Mode** is an optional
HDMI ALLM toggle for both AML and VFM. It restores the previous HDMI signalling
before Kodi returns, including launch failures and watchdog recovery. Displays
must advertise ALLM support and allow automatic game mode. The setting defaults
to on and can be switched off; explicitly saved off preferences are preserved.

The workflow builds stock kernel dependencies to obtain matching modules and
symbol CRCs, validates their dependency order and architectures, and loads the
actual ARM32 userspace plugin under QEMU before publishing the VFM addon.
See [experimental-vfm.md](moonlight/docs/experimental-vfm.md) for the pipeline and
device checks. QEMU does not establish device streaming, teardown or latency.

**Advanced settings → Enable Moonlight Debug Output** controls streaming health
logging and is off by default. When enabled, the next add-on build records one
timestamped health sample per second
in the existing Luna session log, even without the visual overlay. It combines
AML video/decoder queues, transport packet rates, ALSA timing/errors, CPU load and
temperature. VFM includes transport/audio/system readings with AML detail marked
unavailable. See [the field reference](moonlight/docs/stream-diagnostics-coreelec-ng.md).
Unimplemented video recovery/error counters show unavailable, and duplicate OSD
text rows are suppressed. The workflow tests headless monitoring and audio diagnostic failure paths before
starting the native build. No new add-on release is published by this source update.

## Build environment

The workflow uses standard public `ubuntu-24.04` runners. It builds with Pannal's
pinned CE21 NG recipes (`PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm`) and public,
pinned kernel/media sources. A VFM build compiles the stock kernel as an
intermediate build dependency; only selected modules enter the runtime bundle.
Public Moonlight common-code and controller-map
dependencies are checked out at the revisions used by the source snapshot.

Toolchain and dependency checkpoints have a separate `luna-public-ng-v1` cache
namespace; existing Kodi build caches are not modified. Prototype and standard
dependency caches are separated. The Moonlight package is cleaned before each
compile. `clean_build` skips cache restore/save. Native output, source provenance,
ZIP checksums and compact build logs are retained for three days.

`source-lock.json` records the exact exported Moonlight and Luna revisions.
Luna NG 2.7.1.308 was tested on the device: AML and VFM both launch, but VFM
shows visible pixel corruption. This snapshot includes the bridge version 5
linear-frame correction and retains the .308 recovery fixes. No new native
build or add-on release has been made for this correction; picture quality
still needs hardware retesting after the next build.

Before the expensive native job, the workflow exercises VFM frame ownership and
worker failures using host FFmpeg, injects audio initialization failures, checks
runtime packaging and runs Luna's launcher/restore tests. It still loads the
actual ARM32 plugin under QEMU after a native build. See
[the branch audit](moonlight/docs/coreelec-ng-audit-2026-10-28.md) for coverage and
remaining device checks. A cold build can exceed a single job's time budget;
successful intermediate checkpoints allow a later manual run to continue.

To refresh the snapshot, export final tracked files without `.git` metadata
into the source subdirectories, retain the public dependency pins, recompute `source-lock.json`,
and commit the result on the build branch. Do not merge the development branches.
