#!/usr/bin/env bash
# Usage: bash packaging/coreelec-ng/prepare-build.sh /path/to/pannal/CoreELEC
# Prepare a reversible local-source overlay. No builds or network calls here.
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ce="${1:-}"
if [[ -z "$ce" || ! -d "$ce/packages/addons/addon-depends/moonlight-embedded" ]]; then
  echo "Pass the root of an existing pannal/CoreELEC checkout" >&2
  exit 2
fi
ce="$(cd "$ce" && pwd)"
target="$ce/packages/addons/addon-depends/moonlight-embedded"
source_dir="$ce/sources/moonlight-embedded/moonlight-embedded-local"

if [[ -e "$source_dir" && "$(cd "$source_dir" && pwd)" != "$source_root" ]]; then
  echo "Source destination exists and is not this checkout: $source_dir" >&2
  exit 1
fi

if [[ ! -f "$source_root/third_party/moonlight-common-c/src/Limelight.h" ]]; then
  echo "Moonlight submodule missing. Run in $source_root:" >&2
  echo "  git submodule update --init --recursive" >&2
  exit 1
fi

mkdir -p "$(dirname "$source_dir")"
if [[ ! -e "$source_dir" ]]; then
  ln -s "$source_root" "$source_dir"
fi

# Keep backups outside the CoreELEC package's active patch directory.
backup="$target/moonlight-ng-original"
mkdir -p "$backup"
if [[ ! -e "$backup/package.mk" ]]; then
  cp "$target/package.mk" "$backup/package.mk"
fi
if [[ -f "$target/patches/01-surround-fix.patch" ]]; then
  mv "$target/patches/01-surround-fix.patch" "$backup/01-surround-fix.patch"
fi
cp "$source_root/packaging/coreelec-ng/package.mk" "$target/package.mk"
# Pannal's pinned rtmpdump archive vanished from upstream mirrors. Its exact
# bytes are retained by MacPorts with the SAME SHA256; patch only the URL.
bash "$source_root/packaging/coreelec-ng/fix-rtmpdump-source.sh" "$ce"
# Runner-independent x86-64 host toolchain: reused binaries must not contain
# CPU instructions exclusive to the GitHub runner where they were built.
bash "$source_root/packaging/coreelec-ng/patch-host-flags.sh" "$ce"
if [[ "${MOONLIGHT_BUILD_VFM:-no}" == yes ]]; then
  bash "$source_root/packaging/coreelec-ng/vfm/prepare-build.sh" "$ce"
fi

echo "Prepared CoreELEC 21 NG Moonlight overlay."
echo "Checkout: $ce"
echo "Source:   $source_root"
echo "Build:    PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm ./scripts/build moonlight-embedded"
echo "Restore:  cp '$backup/package.mk' '$target/package.mk'"
echo "          mv '$backup/01-surround-fix.patch' '$target/patches/'  # if present"
