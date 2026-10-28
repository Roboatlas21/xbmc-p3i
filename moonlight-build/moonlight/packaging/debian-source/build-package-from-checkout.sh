#!/usr/bin/env bash
# Build a Debian Moonlight package from a source tree already checked out
# by GitHub Actions. This script runs inside the packaging Docker image.
# No repository token or private Git remote is passed to the container.
set -euo pipefail

source_dir="${SOURCE_DIR:-/opt/moonlight-src}"
out_dir="${OUT_DIR:-/out}"
build_dir="/opt/build"

for file in \
  "$source_dir/CMakeLists.txt" \
  "$source_dir/third_party/moonlight-common-c/src/Limelight.h" \
  "$source_dir/third_party/SDL_GameControllerDB/gamecontrollerdb.txt" \
  "/opt/debian/control"; do
  if [[ ! -f "$file" ]]; then
    echo "Missing source or build dependency: $file" >&2
    exit 1
  fi
done

version="$(sed -nE 's/^project\(moonlight-embedded VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)$/\1/p' "$source_dir/CMakeLists.txt")"
if [[ -z "$version" ]]; then
  echo "Cannot read Moonlight version from the checked-out source" >&2
  exit 1
fi

echo "Packaging local Moonlight source: $version"
echo "Commit supplied by Actions: ${SOURCE_GIT_SHA:-unspecified}"

mkdir -p "$build_dir" "$out_dir"
archive="$build_dir/moonlight-embedded_${version}.orig.tar.gz"
pkg_dir="$build_dir/moonlight-embedded-$version"
mkdir -p "$pkg_dir"

# Archive the exact authenticated checkout, including its public submodules.
# Avoid packaging any checkout-local Git metadata/credentials.
tar --exclude-vcs -czf "$archive" -C "$source_dir" .
tar -xzf "$archive" -C "$pkg_dir"

cp -r /opt/debian "$pkg_dir/debian"

if [[ "${TARGET:-}" == "rpi" && "${DISTRO:-}" == "bullseye" ]]; then
  extra_build_deps='libraspberrypi-dev | rbp-userland-dev-osmc'
  extra_deps='libraspberrypi0 | rbp-userland-osmc'
else
  extra_build_deps=''
  extra_deps=''
fi

sed -i "s/EXTRA_BUILD_DEPS/$extra_build_deps/g" "$pkg_dir/debian/control"
sed -i "s/EXTRA_DEPS/$extra_deps/g" "$pkg_dir/debian/control"

cd "$pkg_dir"
debuild -us -uc

# Mirror upstream packaging output, but avoid recursively copying source trees.
shopt -s nullglob
packages=("$build_dir"/*.deb "$build_dir"/*.ddeb)
if (( ${#packages[@]} == 0 )); then
  echo "No Debian packages were generated" >&2
  exit 1
fi
cp -v "${packages[@]}" "$out_dir/"
