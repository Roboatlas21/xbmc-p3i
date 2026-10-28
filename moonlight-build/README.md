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
live on the dedicated build branch. Nothing builds or installs Kodi or a
CoreELEC image. No private repository access token is needed.

The default build produces the standard ARM32 Amlogic native bundle and an
installable `luna-coreelec21-ng.zip` in the **luna-coreelec21-ng-addon** artifact.
It includes the latest configurable video-pipeline setting. Standard Amlogic
remains selected by default. The experimental option reports that its runtime
is unavailable until the full optional bundle has been built and supplied.

`build_vfm_plugin` additionally compiles the experimental FFmpeg/VFM userspace
plugin against the pinned CoreELEC FFmpeg. Its output is a **separate compile-check
artifact**, not an installable VFM runtime. It does not supply private patched
FFmpeg, the VFM kernel bridge, or compatible 4.9 decoder modules. See
[experimental-vfm.md](moonlight/docs/experimental-vfm.md) for those remaining
build and hardware-validation gates. A successful workflow does not establish
VFM compatibility or streaming performance on the device.

## Build environment

The workflow uses standard public `ubuntu-24.04` runners. It builds with Pannal's
pinned CE21 NG recipes (`PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm`) and public,
pinned kernel/media sources. The kernel source supplies build headers; no OS
image is produced or installed. Public Moonlight common-code and controller-map
dependencies are checked out at the revisions used by the source snapshot.

Toolchain and dependency checkpoints have a separate `luna-public-ng-v1` cache
namespace; existing Kodi build caches are not modified. Prototype and standard
dependency caches are separated. The Moonlight package is cleaned before each
compile. `clean_build` skips cache restore/save. Native output, source provenance,
ZIP checksums and compact build logs are retained for three days.

The public runner has completed and cached the toolchain and dependency stages;
the complete Moonlight/Luna build still needs verification after the source-copy
fix. A cold CoreELEC toolchain/dependency build can exceed a single job's time budget;
successful intermediate checkpoints allow a later manual run to continue.

To refresh the snapshot, export final files without `.git` or root `.github`
directories, retain the public dependency pins, recompute `source-lock.json`,
and commit the result on the build branch. Do not merge the development branches.
