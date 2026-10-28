# Luna NG public build snapshot

This directory exports Moonlight and Luna source files without importing their
development histories. The initial snapshot's parent belongs to this public
Kodi fork; its existing upstream history remains intact. Exact source and
dependency revisions are recorded in `source-lock.json`. Source licenses and
assets are retained. The original repositories' workflows are omitted.

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
CoreELEC's device tree disables it. After replacing a previously loaded older
bridge, reboot CoreELEC once; Luna detects this case before stopping Kodi.

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

The source export matches Moonlight `32af94a9b546a43cd2156c271c7ce3d8abe8b50f`;
Luna stays at `5d568b629981fa31cf4dfc49cb399e971d9d050e`. Public run #7 compiled
ARM32 Moonlight and all VFM dependencies and collected the complete runtime
(17 modules, four private libraries). Its QEMU test then failed to find the
ARM loader because CoreELEC's compiler sysroot uses `/usr/lib` without the OS
image's `/lib` symlink. The corrected smoke script invokes that loader directly.
A separate local ARM32 build of patched FFmpeg, libdrm and the VFM plugin passes
the QEMU decoder probe; missing loaders/libraries still fail. ZIP assembly tests
also pass. The full public workflow and actual device streaming remain unverified.
A cold build can exceed
a single job's time budget;
successful intermediate checkpoints allow a later manual run to continue.

To refresh the snapshot, export final files without `.git` or root `.github`
directories, retain the public dependency pins, recompute `source-lock.json`,
and commit the result on the build branch. Do not merge the development branches.
