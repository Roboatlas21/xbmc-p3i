#!/usr/bin/env bash
# Register private FFmpeg/bridge recipes in a build tree, never on a device.
set -euo pipefail
if (( $# != 1 )); then
  echo "Usage: $0 COREELEC_SOURCE" >&2
  exit 2
fi
ce=$(cd "$1" && pwd)
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
patchfile="$ce/packages/multimedia/ffmpeg/patches/v4l2-drmprime/ffmpeg-001-v4l2-drmprime.patch"
expected=77f593d3844f592c07fe53c9958161c01aa59c674f1320a35574655a31051760
actual=$(sha256sum "$patchfile")
[[ ${actual%% *} == "$expected" ]] || { echo 'Unexpected CE DRM PRIME patch' >&2; exit 1; }
packages="$ce/packages/addons/addon-depends"
mkdir -p "$packages/moonlight-vfm-ffmpeg/patches" "$packages/moonlight-vfm-driver"
cp "$here/ffmpeg-package.mk" "$packages/moonlight-vfm-ffmpeg/package.mk"
cp "$patchfile" "$packages/moonlight-vfm-ffmpeg/patches/001-drm-prime.patch"
cp "$here/ffmpeg-nonblocking.patch" "$packages/moonlight-vfm-ffmpeg/patches/002-nonblocking.patch"
cp "$here/driver-package.mk" "$packages/moonlight-vfm-driver/package.mk"
echo 'Prepared private VFM FFmpeg and stock-kernel bridge recipes.'
