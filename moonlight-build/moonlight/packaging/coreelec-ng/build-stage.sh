#!/usr/bin/env bash
# Run one checkpoint of Pannal's native build with bounded GitHub log output.
# Usage: build-stage.sh toolchain|deps-first|deps-final|vfm|moonlight CORE_ELEC_ROOT LOG_DIR
set -euo pipefail

if (( $# != 3 )); then
  echo "Usage: $0 {toolchain|deps-first|deps-final|vfm|moonlight} CORE_ELEC_ROOT LOG_DIR" >&2
  exit 2
fi
stage="$1"
ce="$(cd "$2" && pwd)"
mkdir -p "$3"
logs="$(cd "$3" && pwd)"
recipe="$ce/packages/addons/addon-depends/moonlight-embedded/package.mk"
if [[ ! -f "$recipe" || ! -x "$ce/scripts/build" ]]; then
  echo "Missing Pannal CoreELEC package recipe or scripts/build: $ce" >&2
  exit 2
fi

summary() {
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    printf '%s\n' "$*" >> "$GITHUB_STEP_SUMMARY"
  fi
}

# Read only the literal dependency list (do not source a package recipe here).
mapfile -t definitions < <(grep -E '^PKG_DEPENDS_TARGET="[A-Za-z0-9_+. :\-]*"$' "$recipe" || true)
if (( ${#definitions[@]} != 1 )); then
  echo "Expected one literal PKG_DEPENDS_TARGET in $recipe" >&2
  exit 2
fi
list="${definitions[0]#*=}"
list="${list#\"}"
list="${list%\"}"
read -r -a all_deps <<< "$list"
if (( ${#all_deps[@]} < 2 )) || [[ "${all_deps[0]}" != toolchain ]]; then
  echo "Expected toolchain and one or more other target dependencies" >&2
  exit 2
fi
for pkg in "${all_deps[@]}"; do
  if [[ ! "$pkg" =~ ^[A-Za-z0-9][A-Za-z0-9_.:+-]*$ ]]; then
    echo "Invalid dependency name: $pkg" >&2
    exit 2
  fi
done

# Split the package's actual dependencies in two stable stages; changes to
# package.mk invalidate the dependency cache key in the workflow.
deps=("${all_deps[@]:1}")
midpoint=$(( (${#deps[@]} + 1) / 2 ))
case "$stage" in
  toolchain)  packages=(toolchain) ;;
  deps-first) packages=("${deps[@]:0:midpoint}") ;;
  deps-final) packages=("${deps[@]:midpoint}") ;;
  vfm)        packages=(moonlight-vfm-ffmpeg moonlight-vfm-driver) ;;
  moonlight)  packages=(moonlight-embedded) ;;
  *) echo "Unknown stage: $stage" >&2; exit 2 ;;
esac
if (( ${#packages[@]} == 0 )); then
  echo "Unexpected empty build stage: $stage" >&2
  exit 2
fi

summary "### CoreELEC NG: $stage"
summary "Packages: \`$(printf '%s ' "${packages[@]}")\`"
echo "Stage $stage: ${packages[*]}"

for pkg in "${packages[@]}"; do
  logfile="$logs/${stage}-${pkg//:/_}.log"
  start=$SECONDS
  echo "Building $pkg (full output saved; progress every 60s)"
  (
    while sleep 60 >/dev/null 2>&1; do
      echo "[$pkg] still building, ${SECONDS}s elapsed; free disk: $(df -hP "$ce" | awk 'NR==2 {print $4}')"
    done
  ) &
  heartbeat=$!
  set +e
  (cd "$ce" && ./scripts/build "$pkg") > "$logfile" 2>&1
  status=$?
  set -e
  kill "$heartbeat" 2>/dev/null || true
  wait "$heartbeat" 2>/dev/null || true
  elapsed=$(( SECONDS - start ))
  size="$(wc -c < "$logfile" | tr -d ' ')"

  if (( status == 0 )); then
    # Keep successful package tails, not thousands of compiler command lines.
    tail -n 15 "$logfile" > "$logs/${stage}-${pkg//:/_}.tail.txt"
    rm -f "$logfile"
    echo "PASS $pkg (${elapsed}s; ${size} bytes of output)"
    summary "- PASS \`$pkg\` — ${elapsed}s"
    continue
  fi

  echo "::error title=ARM32 build failed::Stage $stage, package $pkg exited with status $status"
  summary "- **FAILED** \`$pkg\` — exit code $status after ${elapsed}s"
  summary "- See the \`coreelec-ng-build-logs\` workflow artifact for the full failed package log."
  echo "---- Possible first actionable errors ($pkg) ----"
  grep -Einm 12 'fatal error:|(^|[[:space:]])error:|configure: error|CMake Error|undefined reference|No space left on device|cannot find|not found|Error [0-9]+|FAILED:' "$logfile" | cut -c1-400 || true
  echo "---- Final 60 log lines ($pkg) ----"
  tail -n 60 "$logfile" | cut -c1-400
  echo "---- Disk usage ----"
  df -h "$ce"
  # Successful package logs have already been compacted. Compress only the
  # failure's full log so the diagnostic artifact is still practical to keep.
  gzip -1 "$logfile" || true
  exit "$status"
done
