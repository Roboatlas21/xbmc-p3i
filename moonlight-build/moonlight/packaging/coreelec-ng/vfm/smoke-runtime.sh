#!/usr/bin/env bash
# Run the actual ARM32 plugin under QEMU; no devices or kernel modules are used.
set -euo pipefail
if (( $# != 2 )); then
  echo "Usage: $0 COREELEC_SOURCE VFM_DIRECTORY" >&2
  exit 2
fi
ce=$(cd "$1" && pwd)
runtime=$(cd "$2" && pwd)
here=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
shopt -s nullglob
compilers=("$ce"/build.CoreELEC-Amlogic-ng.arm-21*/toolchain/bin/arm*-gcc)
(( ${#compilers[@]} == 1 )) || { echo 'Ambiguous ARM32 compiler' >&2; exit 1; }
cc=${compilers[0]}
sysroot=$("$cc" -print-sysroot)
test -d "$sysroot"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$cc" -Wall -Wextra -Werror "$here/tests/vfm_runtime_probe.c" -ldl -o "$work/probe"
qemu-arm -L "$sysroot" -E "LD_LIBRARY_PATH=$runtime/lib:$sysroot/usr/lib:$sysroot/lib" \
  "$work/probe" "$runtime/lib/libmoonlight-aml-vfm.so"
