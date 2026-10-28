# Luna NG public build snapshot

The final Moonlight and Luna files are exported here in one snapshot commit.
Their development histories are not imported. This public Kodi fork retains its
existing upstream history. Source licenses and assets are retained; the source
repositories' workflows are omitted. Exact file identities and dependency
revisions are recorded in `source-lock.json`.

## Manual build

Open **Actions → Luna NG public snapshot build → Run workflow**. Leave the
workflow branch on `aml-4.9-21.3` and `snapshot_ref` on
`build/luna-ng-snapshot`, or enter a specific snapshot commit. Preparation
resolves that ref once; the native job checks out that same immutable commit.

The workflow is also registered on the default branch so GitHub displays the
manual button. Only that workflow file is added there. Source snapshots live on
the dedicated build branch. No push, PR, schedule or workflow-run trigger starts
this build. No private repository token is needed.

The default build produces the standard ARM32 Amlogic native bundle and an
installable `luna-coreelec21-ng.zip` in the **luna-coreelec21-ng-addon** artifact.
The add-on includes the latest configurable video-pipeline setting, with standard
Amlogic selected by default. The experimental option remains unavailable until
the full optional runtime is built and supplied.

`build_vfm_plugin` also compiles the experimental FFmpeg/VFM userspace plugin
against the pinned CoreELEC FFmpeg. It produces a **separate compile-check
artifact**, not an installable VFM runtime. Private patched FFmpeg, the VFM kernel
bridge and compatible 4.9 decoder modules are not included. See
[experimental-vfm.md](moonlight/docs/experimental-vfm.md) for the remaining build
and device checks. A successful workflow does not prove VFM hardware support.

## Build environment

Standard public `ubuntu-24.04` runners build with pinned Pannal CE21 NG recipes:
`PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm`. The public kernel/media checkouts
provide native build inputs. No Kodi build or CoreELEC image is produced or
installed. Moonlight common code (including its recursive public dependencies)
and controller mappings use the exact revisions from the source snapshot.

Toolchain/dependency checkpoints use a separate `luna-public-ng-v1` cache
namespace. Standard and prototype dependency caches are separated; existing Kodi
build caches are not modified. The Moonlight package is cleaned before compiling
each snapshot. `clean_build` skips cache restore/save. The add-on, native bundle,
provenance, ZIP checksums and compact logs are retained for three days.

The workflow has not been run. The 47 Luna tests passed locally against the
exported sources. A cold CoreELEC build can exceed one job's time budget;
successful intermediate checkpoints let a later manual run continue.

To refresh this export, copy final source files without their Git directories or
root workflows, update the file identities and revisions in `source-lock.json`,
and make a snapshot commit. Do not merge the development branches.
