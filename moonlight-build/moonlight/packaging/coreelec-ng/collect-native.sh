#!/usr/bin/env bash
# Collect an ARM32 Moonlight build from Pannal's CoreELEC build tree.
# Deliberately do not copy system packages into the Kodi add-on.
set -euo pipefail

if (( $# != 2 )); then
  echo "Usage: $0 /path/to/CoreELEC /path/to/output" >&2
  exit 2
fi

# A manually collected artifact must name its exact native source revision.
# Never silently produce an untraceable native test bundle.
if [[ ! "${SOURCE_GIT_SHA:-}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "SOURCE_GIT_SHA must be a full lowercase 40-hex source commit before collecting native artifacts" >&2
  exit 2
fi

ce="$(cd "$1" && pwd)"
out="$2"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/bin/coreelec-ng" "$stage/lib/coreelec-ng"

# Native Moonlight's get_path("gamecontrollerdb.txt") first looks in the
# executable's working directory. Kodi ZIPs do not install /usr/share/moonlight,
# so ship the SDL controller map next to the NG executable.
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
db="$source_root/third_party/SDL_GameControllerDB/gamecontrollerdb.txt"
if [[ ! -s "$db" ]]; then
  echo "Missing required SDL GameControllerDB submodule content" >&2
  exit 1
fi
cp "$db" "$stage/bin/coreelec-ng/gamecontrollerdb.txt"

is_arm32() {
  local f="$1"
  local header
  header="$(readelf -h "$f" 2>/dev/null)" || return 1
  grep -Fq 'Class:                             ELF32' <<<"$header" &&
  grep -Eq 'Machine:[[:space:]]+ARM([[:space:]]|$)' <<<"$header"
}

# Only select artifacts from THIS Moonlight package's build directory.
# A broad find across CoreELEC/build.* could silently package Pannal's old
# libmoonlight-common-c or another package's same-named libraries.
pkg_roots=()
for root in "$ce"/build.*/build/moonlight-embedded-2.7.1-ng-local; do
  [[ -d "$root" ]] && pkg_roots+=("$root")
done
if (( ${#pkg_roots[@]} != 1 )); then
  echo "Expected exactly one CoreELEC NG Moonlight package build root; found ${#pkg_roots[@]}" >&2
  exit 1
fi
package_root="${pkg_roots[0]}"
# CMake build outputs can retain absolute build RUNPATHs. Pannal's
# source_package() sets PKG_INSTALL=$BUILD/install_pkg/<name>-<version>
# for a normal target package (scripts/build uses DESTDIR=$INSTALL).
# image/system is reserved for assembling the overall OS image and need
# not exist when building only the Moonlight package.
build_root="${package_root%/build/moonlight-embedded-2.7.1-ng-local}"
install_root="$build_root/install_pkg/moonlight-embedded-2.7.1-ng-local"
if [[ ! -d "$install_root/usr/bin" || ! -d "$install_root/usr/lib" ||
      ! -f "$install_root/.libreelec-package" ]] ||
   ! grep -Fxq 'INFO_PKG_NAME="moonlight-embedded"' "$install_root/.libreelec-package"; then
  echo "CoreELEC's Moonlight install_pkg staging directory is missing or incomplete: $install_root" >&2
  exit 1
fi
echo "Verified package build: $package_root"
echo "Collecting relocatable installed binaries from: $install_root"
moonlight=""
while IFS= read -r -d '' file; do
  if is_arm32 "$file"; then
    moonlight="$file"
    break
  fi
done < <(find "$install_root/usr/bin" -maxdepth 1 -type f -name moonlight -print0 2>/dev/null || true)

if [[ -z "$moonlight" ]]; then
  echo "No ARM ELF32 Moonlight executable found in the CoreELEC build tree" >&2
  exit 1
fi
cp "$moonlight" "$stage/bin/coreelec-ng/moonlight"
chmod 0755 "$stage/bin/coreelec-ng/moonlight"

# Only Moonlight's own shared objects belong in the add-on's private /lib.
# libamcodec and the other platform libraries must come from CoreELEC NG.
for name in libmoonlight-aml.so libgamestream.so.4 libmoonlight-common.so.4; do
  picked=""
  while IFS= read -r -d '' file; do
    if is_arm32 "$file"; then
      picked="$file"
      break
    fi
  done < <(find "$install_root/usr/lib" -maxdepth 1 \
    \( -type f -o -type l \) -name "$name" -print0 2>/dev/null || true)

  if [[ -z "$picked" ]]; then
    echo "Missing ARM ELF32 shared object: $name" >&2
    exit 1
  fi
  cp -L "$picked" "$stage/lib/coreelec-ng/$name"
done

# libCEC is loaded at runtime through ceccloader (dlopen), so it does not
# appear in Moonlight's DT_NEEDED list. CoreELEC's libcec package installs
# libcec.so.6 as a /var/lib override symlink, but the immutable runtime image
# can leave that target absent. Bundle the exact ARM32 libCEC payload from the
# same CoreELEC build so Luna's private LD_LIBRARY_PATH can satisfy the loader.
libcec_roots=()
for root in "$ce"/build.*/install_pkg/libcec-*; do
  if [[ -d "$root" && -f "$root/.libreelec-package" ]] &&
     grep -Fxq 'INFO_PKG_NAME="libcec"' "$root/.libreelec-package"; then
    libcec_roots+=("$root")
  fi
done
if (( ${#libcec_roots[@]} != 1 )); then
  echo "Expected exactly one CoreELEC libcec install root; found ${#libcec_roots[@]}" >&2
  exit 1
fi
libcec_root="${libcec_roots[0]}"
libcec=""
while IFS= read -r -d '' file; do
  if is_arm32 "$file"; then
    libcec="$file"
    break
  fi
done < <(find "$libcec_root/usr/lib" -maxdepth 1 -type f \
  -name 'libcec.so.6.*' -print0 2>/dev/null || true)

if [[ -z "$libcec" ]]; then
  echo "Missing ARM ELF32 libcec.so.6 runtime payload" >&2
  exit 1
fi
cp "$libcec" "$stage/lib/coreelec-ng/libcec.so.6"
chmod 0644 "$stage/lib/coreelec-ng/libcec.so.6"

{
  echo "Moonlight CoreELEC 21 NG native bundle"
  echo "SOURCE_GIT_SHA=${SOURCE_GIT_SHA}"
  echo "CORE_ELEC_REF=${CORE_ELEC_REF:-unknown}"
  echo "PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm"
  echo
  for f in "$stage/bin/coreelec-ng/moonlight" "$stage/lib/coreelec-ng/"*.so*; do
    echo "### ${f#"$stage"/}"
    file "$f"
    readelf -h "$f" | grep -E 'Class:|Machine:|OS/ABI:'
    readelf -d "$f" | grep -E 'NEEDED|SONAME' || true
    sha256sum "$f"
    echo
  done
} > "$stage/build-info.txt"

# Generate machine-readable ABI/SONAME/DT_NEEDED/digest evidence.
python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/runtime_manifest.py" \
  "$stage" "$SOURCE_GIT_SHA"

tar -C "$stage" -czf "$out/moonlight-coreelec21-ng-arm32.tar.gz" .
cp "$stage/build-info.txt" "$out/build-info.txt"
echo "Collected: $out/moonlight-coreelec21-ng-arm32.tar.gz"
