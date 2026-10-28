#!/usr/bin/env bash
# Requires the exact prepared kernel OUTPUT tree including Module.symvers.
set -euo pipefail
if (( $# != 2 )); then
  echo "Usage: $0 KERNEL_BUILD_TREE AARCH64_CROSS_PREFIX" >&2
  exit 2
fi
kernel=$(cd "$1" && pwd)
here=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
for file in .config Module.symvers include/generated/autoconf.h; do
  [[ -s "$kernel/$file" ]] || { echo "Missing exact kernel build file: $file" >&2; exit 1; }
done
for symbol in vf_reg_provider vf_unreg_provider vf_provider_init vf_get_provider \
              vf_notify_receiver vf_notify_receiver_by_name vfm_map_add vfm_map_remove set_blackout_policy dma_buf_get \
              dma_buf_attach dma_buf_map_attachment platform_device_register_full \
              platform_device_unregister bus_find_device platform_bus_type \
              of_find_compatible_node of_device_is_available \
              of_find_device_by_node dummy_dma_ops; do
  awk -v name="$symbol" '$2 == name { found=1 } END { exit !found }' \
    "$kernel/Module.symvers" || { echo "Kernel lacks export: $symbol; do not patch the OS" >&2; exit 1; }
done
grep -qx 'CONFIG_ARM64=y' "$kernel/.config"
grep -qx 'CONFIG_COMPAT=y' "$kernel/.config"
make -C "$kernel" ARCH=arm64 CROSS_COMPILE="$2" \
  M="$here/kernel/moonlight-vfm" modules
