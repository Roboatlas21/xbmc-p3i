# CoreELEC 21 NG ARM32 native build

The `.github/workflows/coreelec-ng-native.yml` workflow builds only the ARM32
Moonlight package for Pannal's CoreELEC 21 NG (`PROJECT=Amlogic-ce`,
`DEVICE=Amlogic-ng`, `ARCH=arm`). It does not build or flash an operating-system
image. The source-check workflow stays separate and runs on every push.

## Running it

Open **Actions > CoreELEC 21 NG ARM32 native build > Run workflow** and select
`feat/coreelec-ng`.

- Default (`clean_build=false`): restore the most complete compatible checkpoint,
  build what is missing, then compile Moonlight.
- `clean_build=true`: ignore existing checkpoints and rerun all stages. This
  diagnostic mode intentionally does not upload new reusable checkpoints.
- Changing `.github/workflows/coreelec-ng-native.yml` on the development branch
  also triggers the native workflow. Ordinary C/Python changes do not
  automatically start this expensive build.

Stages are a compiled toolchain, first half of dependencies, remaining
packages, then Moonlight. The dependency halves are read from
`PKG_DEPENDS_TARGET` in the checked-out package recipe; the script does not
maintain a second hardcoded package list. `packaging/coreelec-ng/build-stage.sh`
prints progress heartbeats and only a brief tail of each successful package's
log. It prints likely first errors plus the final 60 lines on a failure.
The **coreelec-ng-build-logs** artifact keeps short success tails and the
compressed full log for a failed package. Logs are available even if a
compilation stage fails. The Actions job summary identifies the failed package.

## Cache model

Pannal's `scripts/build` uses package stamp files to skip unchanged packages.
The three immutable GitHub Actions checkpoints therefore preserve the
`CoreELEC/build.*` directories (including stamps and installed compiler/sysroot
files), rather than saving downloads or ccache alone:

1. The compiled toolchain.
2. Toolchain plus first half of Moonlight's dependencies.
3. All dependencies required by Moonlight, before compiling Moonlight itself.

The workflow restores the most complete checkpoint available, then builds and
saves later checkpoints as it progresses. Compiled Moonlight is **not** included
in the dependency checkpoint, so native source revisions compile against the
same validated build environment.

Cache keys include the exact checked-out Pannal CoreELEC, Linux and media-module
commit IDs, the Moonlight package recipe hash (for dependency snapshots), the
ARM32 target and a cache layout version. They intentionally do **not** include
every Moonlight source commit. On a major build environment change, bump the
`cache-layout-v2` string in the workflow. Do not use partial-prefix cache
restores from incompatible toolchains.

GitHub-hosted runners are ephemeral. The default repository Actions cache
allowance is 10 GB; a sufficiently large full CoreELEC build state or multiple
snapshots may evict earlier checkpoints or make caching ineffective. Check the
job's **Native build cache** summary and repository cache usage after the first
run. If the archives are too large, use an appropriately sized persistent
self-hosted build worker rather than continually reuploading giant snapshots.
A cache save failure is non-fatal and does not mean compilation passed or failed.

## Results and next step

After a successful native compilation, the workflow calls
`packaging/coreelec-ng/collect-native.sh` to validate and package ARM ELF32
executables and required private shared libraries. It uploads:

- **moonlight-coreelec21-ng-arm32**: `.tar.gz` native files and `build-info.txt`
- **coreelec-ng-build-logs**: compact diagnostics, with a failed package's
  compressed full log when needed

The ARM32 artifact is **not** yet a Kodi-installable ZIP. The separate private
`Roboatlas21/Luna` repository has `packaging/package_ng.py` to assemble the add-on
from this artifact. GitHub's standard per-repository `GITHUB_TOKEN` cannot
checkout that second private repository automatically. A subsequent packaging
workflow requires an explicitly granted cross-repository credential or an
artifact handoff; this pipeline deliberately avoids an unconfigured secret.

No claim of working video, DRM OSD, or Vortex performance parity can be made
until the new binary is tested on an AM6B+ with CoreELEC 21 NG.

## Repair for missing rtmpdump archive (2026-10-25)

Pannal's CoreELEC 21 recipe pins `rtmpdump` commit
`f1b83c10d8beb43fcc70a6e88cf4325499f25857`, SHA-256
`c68e05989a93c002e3ba8df3baef0021c17099aa2123a9c096a5cc8e029caf95`.
The original `repo.or.cz` snapshot now returns HTML or errors; the default
CoreELEC and LibreELEC mirrors return HTTP 404.

MacPorts' `www/rtmpdump/Portfile` independently documents the **identical**
commit and checksum, with an archive size of exactly **143043 bytes**.

`packaging/coreelec-ng/fix-rtmpdump-source.sh` changes **only** the
`PKG_URL` of this explicitly checked pinned recipe. It refuses to proceed
if Pannal changes the version, checksum, or original URL. It does not
disable integrity checks or update the package.

The native workflow fetches the archive before expensive compilation. It
verifies **size, SHA-256 and tar.gz integrity**, and places the file under
the standard CoreELEC source filename, along with the `.url` and `.sha256`
stamps required by `scripts/get_archive`. A second MacPorts mirror is
tried if the primary fails. No archive is vendored in the repository.

Changing the source patch changes the dependency cache key, **not** the
saved ARM32 toolchain checkpoint. Mock tests confirm the patch is
idempotent and rejects unfamiliar inputs.


## Portable cached host tools — SIGILL repair

A subsequent build restored the initial 2.59 GB toolchain cache but failed
immediately when CoreELEC invoked its cached GNU `sed`, with exit status
132 (SIGILL). Pannal's `config/optimize` compiled host binaries using
`HOST_CFLAGS="-march=native ..."`. GitHub's hosted x86-64 runner CPU
models can differ between builds, making those binaries unsafe to reuse.

The local overlay now replaces that **exact host** flag with
`-march=x86-64 -mtune=generic` before the toolchain is built.
`HOST_CXXFLAGS` inherits this setting. This does not alter the ARM32
target architecture, optimize Moonlight for x86-64, or patch Pannal's
public repository. The script rejects unfamiliar upstream flags rather than
silently applying an unsafe substitute.

The native workflow fingerprints the host-patch script and uses
`cache-layout-v3-portable-x86-64`, so the previous runner-specific
toolchain/dependency caches **cannot be restored**. This forces one
new toolchain build; the new cache is designed to be reusable.

The new `check-host-tools.sh` smoke test runs the actual compiled CoreELEC
`sed` from `build.*/toolchain/bin` and checks common compiled host helpers
after cache restoration or a fresh build and **before saving a checkpoint or
building any dependencies**. A failing tool halts the build immediately.
Unit tests cover idempotence, refusal to change unexpected host flags,
successful host-tool execution and rejection of a simulated SIGILL exit.

## Native ABI and provenance manifest

Artifact collection no longer searches all CoreELEC build and target
directories for same-named files. It selects objects exclusively from
`build.*/build/moonlight-embedded-2.7.1-ng-local`, excluding unrelated
Pannal Moonlight packages and stale system-library installs.

`packaging/coreelec-ng/runtime_manifest.py` validates **the collected
binary bytes**, without running ARM code on the x86-64 builder, and
produces `runtime-manifest.json` inside the native tarball. It records
the exact source revision, target, ELF32 little-endian ARM headers,
per-file SHA-256 and sizes, shared-library SONAMEs and DT_NEEDED entries,
and the executable's dynamic loader. It checks that the executable and
Amlogic library link to the intended bundled Moonlight libraries.
Unexpected embedded RPATH/RUNPATH entries are rejected.

`external_needed` is deliberately a list of **unverified external
runtime requirements**, not proof that they are present on the AM6B+.
The CoreELEC 21 device must independently provide the correct ARM32
libc, libamcodec, libCEC, ALSA/Opus, libdrm, PulseAudio and other
needed libraries. These are not copied from the build host or other
CoreELEC targets.

## Mandatory controller mappings in the portable add-on

Vortex's source ZIP includes `bin/gamecontrollerdb.txt`, and native
Moonlight reads `gamecontrollerdb.txt` from its current working directory
before looking for `/usr/share/moonlight/`. CoreELEC's addon install does
not install that system data file.

The NG collector now copies the pinned SDL_GameControllerDB submodule
database to `bin/coreelec-ng/gamecontrollerdb.txt`. The native manifest
records and hashes it as an asset; Luna's packager verifies this exact
file alongside the four ELF objects. The launcher already uses the selected
binary directory as its working directory.

### Amlogic library dependency correction

Vortex's original AArch64 ELF confirms `libmoonlight-aml.so` is loaded
via `dlopen` and has `DT_NEEDED` on `libamcodec.so` and libc, **not**
necessarily `libgamestream.so.4`. The manifest checks only the essential
Moonlight executable -> libgamestream -> libmoonlight-common link chain.
A private Amlogic module may validly omit a direct libgamestream edge;
requiring it would reject working builds compiled with --as-needed.

## Collect installed, relocatable objects

Pannal's `scripts/build` installs CMake outputs into
`build.*/image/system/usr/bin` and `usr/lib` as well as its shared
toolchain sysroot. CMake's intermediate build-tree executables may
contain a temporary absolute RUNPATH pointing into the build machine;
packaging those directly would break on the AM6B+.

The collector requires one matching
`build.*/build/moonlight-embedded-2.7.1-ng-local` directory for
provenance, then selects **only** installed `moonlight`,
`libmoonlight-aml.so`, `libgamestream.so.4`, and
`libmoonlight-common.so.4` from that build's installed
`image/system` paths. It never scans unrelated target directories or
host system libraries. The manifest still rejects unexpected RPATH and
RUNPATH references; if a build is not relocatable it fails before ZIP
packaging rather than shipping a broken binary.

## Fix for installed-package collection (2026-10-25)

Pannal's `source_package()` sets `PKG_INSTALL` for a normal target package to
`$BUILD/install_pkg/moonlight-embedded-2.7.1-ng-local`. Its
`scripts/build` explicitly overrides `INSTALL` with `PKG_INSTALL` and
uses `DESTDIR=$INSTALL` for CMake installation. Therefore
`build.*/image/system` is **not** a valid assumption for a
package-only native build. This was the exact cause of the failed
ARM32 artifact step in GitHub run #40261028512, even though the
Moonlight compilation succeeded.

The collector now reads only the Moonlight package's versioned
`install_pkg/.../usr/bin` and `usr/lib` after verifying the exact
`.libreelec-package` stamp. This retains the installed/relocatable
artifact requirement, with no fallback to stale CMake build files.
A synthetic end-to-end collector test asserts that the bundle and
runtime manifest come from the installed staging path, not unrelated
`image/system` artifacts, and fails if required installed objects
are missing. It runs in both native preflight and ordinary source CI.
