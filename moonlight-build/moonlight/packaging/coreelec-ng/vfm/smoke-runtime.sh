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
# CoreELEC installs glibc in /usr/lib and adds /lib -> /usr/lib only when
# assembling an OS image. The compiler sysroot has no /lib loader for QEMU
# to resolve the probe's PT_INTERP. Invoke its target loader explicitly.
loader="$sysroot/usr/lib/ld-linux-armhf.so.3"
if [[ ! -f "$loader" ]]; then
  echo "Missing CoreELEC ARM32 runtime loader: $loader" >&2
  exit 1
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"$cc" -Wall -Wextra -Werror "$here/tests/vfm_runtime_probe.c" -ldl -o "$work/probe"
qemu-arm -L "$sysroot" "$loader" --inhibit-cache \
  --library-path "$runtime/lib:$sysroot/usr/lib:$sysroot/lib" \
  "$work/probe" "$runtime/lib/libmoonlight-aml-vfm.so"
