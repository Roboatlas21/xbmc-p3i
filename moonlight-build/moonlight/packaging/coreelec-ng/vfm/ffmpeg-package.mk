# SPDX-License-Identifier: GPL-3.0-or-later
# Add-on-private FFmpeg; nothing is installed over CoreELEC's system FFmpeg.
PKG_NAME="moonlight-vfm-ffmpeg"
PKG_VERSION="8.1.2"
PKG_SHA256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
PKG_LICENSE="LGPL-2.1-or-later"
PKG_SITE="https://ffmpeg.org"
PKG_URL="https://ffmpeg.org/releases/ffmpeg-${PKG_VERSION}.tar.xz"
PKG_DEPENDS_TARGET="toolchain libdrm"
PKG_TOOLCHAIN="manual"
PKG_LONGDESC="Private ARM32 V4L2/DRM PRIME decoder libraries for Moonlight VFM"

configure_target() {
  "${PKG_BUILD}/configure" --prefix=/usr/lib/moonlight-vfm \
    --target-os=linux --arch="${TARGET_ARCH}" --cpu="${TARGET_CPU}" \
    --enable-cross-compile --cross-prefix="${TARGET_PREFIX}" \
    --sysroot="${SYSROOT_PREFIX}" --cc="${CC}" --ar="${AR}" --nm="${NM}" \
    --host-cc="${HOST_CC}" --pkg-config="${TOOLCHAIN}/bin/pkg-config" \
    --extra-cflags="${CFLAGS}" --extra-ldflags="${LDFLAGS}" \
    --disable-everything --disable-autodetect --disable-programs --disable-doc \
    --disable-static --enable-shared --enable-pic --enable-neon \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample \
    --disable-swscale --enable-avcodec --enable-avutil --enable-pthreads \
    --enable-libdrm --enable-v4l2_m2m \
    --enable-decoder=h264_v4l2m2m,hevc_v4l2m2m \
    --enable-parser=h264,hevc --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb
}

make_target() {
  make
}

makeinstall_target() {
  # A manual recipe must populate both the compiler sysroot and runtime stage.
  make DESTDIR="${SYSROOT_PREFIX}" install
  make DESTDIR="${INSTALL}" install
  mkdir -p "${INSTALL}/usr/share/licenses/moonlight-vfm"
  cp "${PKG_BUILD}/COPYING.LGPLv2.1" \
    "${INSTALL}/usr/share/licenses/moonlight-vfm/FFmpeg-LGPL-2.1.txt"
}
