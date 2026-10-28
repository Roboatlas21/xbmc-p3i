#!/usr/bin/env bash
# Build an add-on-private FFmpeg. Never install over CoreELEC's FFmpeg.
# Inputs: pristine FFmpeg 8.1.2, pinned CoreELEC source, empty build dir, prefix.
set -euo pipefail
if (( $# != 4 )); then
  echo "Usage: $0 FFMPEG_SOURCE COREELEC_SOURCE BUILD_DIRECTORY PRIVATE_PREFIX" >&2
  exit 2
fi
: "${VFM_CROSS_PREFIX:?Set to the CoreELEC ARM32 toolchain prefix (ending in -)}"
: "${VFM_SYSROOT:?Set to the matching CoreELEC ARM32 sysroot}"
src=$(cd "$1" && pwd)
ce=$(cd "$2" && pwd)
build=$3
prefix=$4
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
patchfile="$ce/packages/multimedia/ffmpeg/patches/v4l2-drmprime/ffmpeg-001-v4l2-drmprime.patch"
expected=77f593d3844f592c07fe53c9958161c01aa59c674f1320a35574655a31051760
actual=$(sha256sum "$patchfile")
[[ ${actual%% *} == "$expected" ]] || { echo "Unexpected CE DRM PRIME patch revision" >&2; exit 1; }
if [[ -e "$build" ]]; then
  echo "Use a new build directory; source/prefix must not be overwritten" >&2
  exit 1
fi
mkdir -p "$build" "$prefix"
build=$(cd "$build" && pwd)
prefix=$(cd "$prefix" && pwd)
[[ "$prefix" != /usr && "$prefix" != / ]] || exit 1
# Work on a private copy, preserving both the original FFmpeg and CE trees.
cp -a "$src/." "$build/"
cd "$build"
patch --batch --fuzz=0 -p1 < "$patchfile"
patch --batch --fuzz=0 -p1 < "$here/ffmpeg-nonblocking.patch"
export PKG_CONFIG_SYSROOT_DIR="$VFM_SYSROOT"
export PKG_CONFIG_LIBDIR="$VFM_SYSROOT/usr/lib/pkgconfig:$VFM_SYSROOT/usr/share/pkgconfig"
./configure --prefix="$prefix" --target-os=linux --arch=arm --cpu=cortex-a53 \
  --enable-cross-compile --cross-prefix="$VFM_CROSS_PREFIX" --sysroot="$VFM_SYSROOT" \
  --extra-cflags='-mfloat-abi=hard -mfpu=neon' \
  --disable-everything --disable-autodetect --disable-programs --disable-doc \
  --disable-static --enable-shared --enable-pic --enable-neon \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample \
  --disable-swscale --enable-avcodec --enable-avutil --enable-pthreads \
  --enable-libdrm --enable-v4l2_m2m \
  --enable-decoder=h264_v4l2m2m,hevc_v4l2m2m \
  --enable-parser=h264,hevc --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb
make -j"${VFM_JOBS:-2}"
make install
echo "Private ARM32 FFmpeg staged at $prefix; no system image changed."
