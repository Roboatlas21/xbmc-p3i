# SPDX-License-Identifier: GPL-3.0-or-later
# Local source overlay for Pannal's CoreELEC 21 NG build tree.
# Copy to packages/addons/addon-depends/moonlight-embedded/package.mk.
PKG_NAME="moonlight-embedded"
PKG_VERSION="2.7.1-ng-local"
PKG_ARCH="any"
PKG_LICENSE="GPLv3"
PKG_SITE="https://github.com/moonlight-stream/moonlight-embedded"
PKG_URL="file://${ROOT}/sources/moonlight-embedded/moonlight-embedded-local"
PKG_SOURCE_NAME="moonlight-embedded-local"
# CoreELEC's directory unpack already includes recursive submodule sources.
# Omit Git metadata at every level, including read-only objects and gitdir files.
PKG_TAR_COPY_OPTS="--exclude=.git"
PKG_NEED_UNPACK="${PKG_URL#file://}/src ${PKG_URL#file://}/libgamestream ${PKG_URL#file://}/cmake ${PKG_URL#file://}/CMakeLists.txt ${PKG_URL#file://}/third_party/moonlight-common-c"
PKG_DEPENDS_TARGET="toolchain curl pulseaudio systemd alsa-lib libevdev sdlgamecontrollerdb opus libamcodec libdrm libcec"
PKG_SECTION=""
PKG_SHORTDESC="Moonlight Embedded for CoreELEC NG (local fork)"
PKG_LONGDESC="Moonlight Embedded built from an independently checked-out local source tree."
PKG_CMAKE_OPTS_TARGET="-DENABLE_FFMPEG=OFF -DENABLE_X11=OFF -DENABLE_CEC=ON -DENABLE_SDL=OFF -DENABLE_CORE_ELEC_AUDIO_MAPPING=ON -DENABLE_DRM_OSD=ON"

# Compile the separate plugin against CE's FFmpeg headers. At runtime the
# optional bundle supplies the patched private FFmpeg; the AML default is
# unchanged and does not link to it. Changing this flag requires a clean rebuild.
if [ "${MOONLIGHT_BUILD_VFM:-no}" = "yes" ]; then
  PKG_DEPENDS_TARGET+=" ffmpeg"
  PKG_NEED_UNPACK+=" ${PKG_URL#file://}/kernel"
  PKG_CMAKE_OPTS_TARGET+=" -DENABLE_AML_VFM=ON"
fi

pre_build_target() {
  # Use moonlight-common-c from the SAME revision as the Moonlight source.
  # Pannal's old separate dependency recipe is pinned to a different commit.
  # Check the actual unpacked tree that CMake will compile.
  local source="${PKG_BUILD}"
  if [ ! -f "${source}/third_party/moonlight-common-c/src/Limelight.h" ] || \
     [ ! -f "${source}/third_party/moonlight-common-c/nanors/rs.c" ] || \
     [ ! -f "${source}/third_party/moonlight-common-c/enet/include/enet/enet.h" ]; then
    echo "Missing moonlight-common-c/nanoRS/ENet submodule content; run git submodule update --init --recursive" >&2
    return 1
  fi
  # Keep moonlight-common-c's unpacked ENet submodule intact; do not recopy it.
  # Upstream common-c pins a Moonlight-specific ENet revision and expects that
  # exact API/ABI; substituting CoreELEC's much older standalone ENet can
  # compile successfully while producing incompatible runtime behaviour.
  mkdir -p "${PKG_BUILD}/third_party/SDL_GameControllerDB"
  cp -a "$(get_build_dir sdlgamecontrollerdb)/." "${PKG_BUILD}/third_party/SDL_GameControllerDB/"
}
