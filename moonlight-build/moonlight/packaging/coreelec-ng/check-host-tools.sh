#!/usr/bin/env bash
# Smoke-check actual restored/built CoreELEC HOST utilities before any package
# build. In particular, a runner-specific GNU sed can crash with SIGILL.
set -euo pipefail

if (( $# != 1 )); then
  echo "Usage: $0 /path/to/CoreELEC" >&2
  exit 2
fi
ce="$(cd "$1" && pwd)"
if ! grep -Fxq 'HOST_CFLAGS="-march=x86-64 -mtune=generic -O2 -Wall -pipe -I$TOOLCHAIN/include"' "$ce/config/optimize"; then
  echo 'Portable HOST_CFLAGS patch is missing; refusing to use cached host tools' >&2
  exit 1
fi
shopt -s nullglob
toolchains=("$ce"/build.*/toolchain)
if (( ${#toolchains[@]} != 1 )) || [[ ! -d "${toolchains[0]}" ]]; then
  echo "Expected precisely one completed CoreELEC build.*/toolchain directory; found ${#toolchains[@]}" >&2
  exit 1
fi
bindir="${toolchains[0]}/bin"
sedbin="$bindir/sed"
if [[ ! -x "$sedbin" ]]; then
  echo "Missing compiled CoreELEC host sed: $sedbin" >&2
  exit 1
fi

# Use the compiled tool directly (not /usr/bin/sed). Test an actual
# substitution, since that's where the previous restored host tool crashed.
if ! result="$(printf 'runner-native\n' | "$sedbin" 's/runner-native/portable/')"; then
  echo "::error title=CoreELEC cached host tool failed::Compiled sed failed to execute on this x86-64 runner; reject checkpoint" >&2
  exit 1
fi
if [[ "$result" != portable ]]; then
  echo 'Compiled CoreELEC sed returned incorrect substitution result' >&2
  exit 1
fi
printf 'PASS: compiled CoreELEC sed can execute substitutions (%s)\n' "$sedbin"

# Other common compiled HOST helpers; missing optional tools are not fatal,
# but any present tool must be executable on this runner.
for tool in make cmake ninja; do
  file="$bindir/$tool"
  if [[ -x "$file" ]]; then
    if ! "$file" --version >/dev/null 2>&1; then
      echo "::error title=CoreELEC cached host tool failed::Compiled $tool failed --version on this runner; reject checkpoint" >&2
      exit 1
    fi
    echo "PASS: compiled host $tool executes"
  fi
done
