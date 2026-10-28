# Pannal CoreELEC 21 NG cross-build

Build target: Ugoos AM6B+, 32-bit ARM userspace, kernel 4.9.

The recipe in this folder is an **overlay**, not a change to pannal's public
repository. It uses a local clone of this *private* Moonlight fork, avoiding
GitHub tarball authentication in Pannal's build system.

## Steps on a Linux build machine

Clone Pannal's CoreELEC tree and this Moonlight fork. In the Moonlight fork:

```sh
git checkout feat/coreelec-ng
git submodule update --init --recursive
bash packaging/coreelec-ng/prepare-build.sh /path/to/CoreELEC
```

Then in the CoreELEC checkout:

```sh
PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm ./scripts/build moonlight-embedded
```

If CoreELEC cached an earlier Moonlight build, clean and rebuild that target
using the **same** PROJECT/DEVICE/ARCH. Re-running the setup script preserves
the original recipe and patch in `moonlight-ng-original`.

## Why the dependencies differ from the stock recipe

Pannal's stock Moonlight package tracks TheChoconut commit
`395f474`; its separate `moonlight-common-c` recipe tracks
`9da6329`. Our source fork starts at `f7dc33c`, whose
`moonlight-common-c` gitlink is `b126e481a195fdc7152d211def17190e3434bcce`.

This build takes moonlight-common-c from the checked-out git submodule
instead of copying the incompatible old package's source. It retains
Pannal's enet, sdlgamecontrollerdb and libamcodec dependencies.

Pannal's `01-surround-fix.patch` is moved out of the patch folder because
the same CoreELEC channel mapping is now an **optional CMake flag** in our
Moonlight fork.

## Limitations

This recipe is **prepared but has not completed a real CoreELEC cross-build**.
The host needs Pannal's full toolchain, local-source dependencies and
associated sources, as described in his README. Do not install any binary
until the build and ARM ELF32/NG sysroot checks pass.
