# SPDX-License-Identifier: GPL-3.0-or-later
PKG_NAME="moonlight-vfm-driver"
PKG_VERSION="3"
PKG_LICENSE="GPL-2.0-only"
PKG_SITE="https://github.com/Roboatlas21/moonlight-embedded"
PKG_URL="file://${ROOT}/sources/moonlight-embedded/moonlight-embedded-local/kernel/moonlight-vfm"
PKG_SOURCE_NAME="moonlight-vfm-local"
PKG_TAR_COPY_OPTS="--exclude=.git"
PKG_DEPENDS_TARGET="toolchain linux media_modules-aml"
PKG_NEED_UNPACK="${PKG_URL#file://} ${LINUX_DEPENDS}"
PKG_TOOLCHAIN="manual"
PKG_LONGDESC="Moonlight DMA-BUF bridge for the existing CoreELEC NG amvideo sink"

pre_make_target() {
  unset LDFLAGS
}

make_target() {
  local kernel="$(kernel_path)"
  # Use the stock configured kernel and its full symbol-version table.
  # modules_prepare by itself is insufficient with CONFIG_MODVERSIONS.
  test -s "${kernel}/Module.symvers"
  grep -qx 'CONFIG_ARM64=y' "${kernel}/.config"
  grep -qx 'CONFIG_COMPAT=y' "${kernel}/.config"
  grep -qx 'CONFIG_MODVERSIONS=y' "${kernel}/.config"
  local symbol
  for symbol in vf_reg_provider vf_unreg_provider vf_provider_init vf_get_provider \
                vf_notify_receiver vf_notify_receiver_by_name vfm_map_add \
                vfm_map_remove set_blackout_policy dma_buf_get dma_buf_attach \
                dma_buf_map_attachment platform_device_register_full \
                platform_device_unregister bus_find_device platform_bus_type \
                of_find_compatible_node of_device_is_available; do
    awk -v name="${symbol}" '$2 == name { found=1 } END { exit !found }' \
      "${kernel}/Module.symvers" || die "Installed-kernel interface missing: ${symbol}"
  done
  kernel_make -C "${kernel}" M="${PKG_BUILD}" modules
}

makeinstall_target() {
  mkdir -p "${INSTALL}/$(get_full_module_dir)/moonlight-vfm"
  cp "${PKG_BUILD}/moonlight_vfm.ko" \
    "${INSTALL}/$(get_full_module_dir)/moonlight-vfm/"
  mkdir -p "${INSTALL}/usr/share/licenses/moonlight-vfm"
  cp "$(kernel_path)/COPYING" "${INSTALL}/usr/share/licenses/moonlight-vfm/kernel-GPL-2.0.txt"
}
