#!/usr/bin/env bash
# CoreELEC NG on ephemeral x86-64 builders: avoid host binaries compiled for
# a runner-specific microarchitecture. Only changes the host compiler flags.
set -euo pipefail

if (( $# != 1 )); then
  echo "Usage: $0 /path/to/CoreELEC" >&2
  exit 2
fi
ce="$(cd "$1" && pwd)"
config="$ce/config/optimize"
if [[ ! -f "$config" ]]; then
  echo "Missing Pannal CoreELEC config/optimize: $config" >&2
  exit 2
fi
if [[ "$(uname -m)" != x86_64 ]]; then
  echo "This portable host-toolchain configuration requires an x86-64 build runner" >&2
  exit 2
fi

python3 - "$config" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
original = 'HOST_CFLAGS="-march=native -O2 -Wall -pipe -I$TOOLCHAIN/include"'
portable = 'HOST_CFLAGS="-march=x86-64 -mtune=generic -O2 -Wall -pipe -I$TOOLCHAIN/include"'
contents = path.read_text()
if contents.count(original) == 1 and portable not in contents:
    path.write_text(contents.replace(original, portable, 1))
elif contents.count(portable) != 1 or original in contents:
    raise SystemExit('CoreELEC HOST_CFLAGS differ from the verified recipe; refusing an unsafe patch')
print('Portable x86-64 host flags: -march=x86-64 -mtune=generic (no -march=native)')
PY
# Pannal derives HOST_CXXFLAGS directly from HOST_CFLAGS. Refuse a conflicting
# downstream change rather than claiming portability that isn't provided.
grep -Fxq 'HOST_CXXFLAGS="$HOST_CFLAGS"' "$config" || {
  echo 'Expected HOST_CXXFLAGS to inherit portable HOST_CFLAGS' >&2
  exit 1
}
