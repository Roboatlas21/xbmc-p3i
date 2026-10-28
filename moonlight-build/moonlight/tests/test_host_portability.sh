#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fix="$repo/packaging/coreelec-ng/patch-host-flags.sh"
check="$repo/packaging/coreelec-ng/check-host-tools.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
ce="$tmp/CoreELEC"
mkdir -p "$ce/config" "$ce/build.CoreELEC-Amlogic-ng.arm-21/toolchain/bin"
cat > "$ce/config/optimize" <<'CONFIG'
HOST_CFLAGS="-march=native -O2 -Wall -pipe -I$TOOLCHAIN/include"
HOST_CXXFLAGS="$HOST_CFLAGS"
CONFIG
bash "$fix" "$ce" > "$tmp/result.txt"
grep -Fxq 'HOST_CFLAGS="-march=x86-64 -mtune=generic -O2 -Wall -pipe -I$TOOLCHAIN/include"' "$ce/config/optimize"
first="$(sha256sum "$ce/config/optimize")"
bash "$fix" "$ce" > /dev/null
[[ "$first" == "$(sha256sum "$ce/config/optimize")" ]]
ln -s "$(command -v sed)" "$ce/build.CoreELEC-Amlogic-ng.arm-21/toolchain/bin/sed"
ln -s "$(command -v make)" "$ce/build.CoreELEC-Amlogic-ng.arm-21/toolchain/bin/make"
bash "$check" "$ce" > "$tmp/smoke.out"
grep -Fq 'PASS: compiled CoreELEC sed' "$tmp/smoke.out"

# Bad sed replacement must be rejected; avoid killing a process with SIGILL
# by simulating the same nonzero status as an incompatible cached tool.
rm "$ce/build.CoreELEC-Amlogic-ng.arm-21/toolchain/bin/sed"
cat > "$ce/build.CoreELEC-Amlogic-ng.arm-21/toolchain/bin/sed" <<'BAD'
#!/bin/sh
exit 132
BAD
chmod +x "$ce/build.CoreELEC-Amlogic-ng.arm-21/toolchain/bin/sed"
if bash "$check" "$ce" > "$tmp/reject.log" 2>&1; then
  echo 'Expected cached tool smoke test to reject a failing sed' >&2
  exit 1
fi
grep -Fq 'Compiled sed failed' "$tmp/reject.log"

# Unknown upstream changes must not be silently overwritten.
sed -i 's/-march=x86-64 -mtune=generic/-march=unknown/' "$ce/config/optimize"
if bash "$fix" "$ce" > "$tmp/reject-flags.log" 2>&1; then
  echo 'Expected unfamiliar HOST_CFLAGS rejection' >&2
  exit 1
fi
grep -Fq 'refusing an unsafe patch' "$tmp/reject-flags.log"
echo 'PASS: portable flags, idempotence, cache smoke test and stale-tool rejection'
