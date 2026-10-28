#!/usr/bin/env bash
# Run the native build logger with a tiny mock CoreELEC package tree.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runner="$here/packaging/coreelec-ng/build-stage.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
ce="$work/CoreELEC"
mkdir -p "$ce/scripts" "$ce/packages/addons/addon-depends/moonlight-embedded"
cat > "$ce/packages/addons/addon-depends/moonlight-embedded/package.mk" <<'RECIPE'
PKG_DEPENDS_TARGET="toolchain curl pulseaudio libamcodec libdrm"
RECIPE
cat > "$ce/scripts/build" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
echo "$1" >> "$MOCK_CALLED"
echo "Compiling $1"
if [[ "$1" == "${MOCK_FAIL:-}" ]]; then
  echo 'configure: error: deliberately missing dependency' >&2
  exit 23
fi
MOCK
chmod +x "$ce/scripts/build"
export MOCK_CALLED="$work/called.txt"
export GITHUB_STEP_SUMMARY="$work/summary.md"

bash "$runner" toolchain "$ce" "$work/logs" > "$work/out.txt"
bash "$runner" deps-first "$ce" "$work/logs" >> "$work/out.txt"
bash "$runner" deps-final "$ce" "$work/logs" >> "$work/out.txt"
bash "$runner" moonlight "$ce" "$work/logs" >> "$work/out.txt"
cat > "$work/expected.txt" <<'EXPECTED'
toolchain
curl
pulseaudio
libamcodec
libdrm
moonlight-embedded
EXPECTED
diff -u "$work/expected.txt" "$MOCK_CALLED"
grep -q 'PASS.*moonlight-embedded' "$work/summary.md"
[[ ! -f "$work/logs/moonlight-moonlight-embedded.log" ]]
[[ -f "$work/logs/moonlight-moonlight-embedded.tail.txt" ]]

: > "$MOCK_CALLED"
export MOCK_FAIL=libdrm
if bash "$runner" deps-final "$ce" "$work/failure" > "$work/failure-output.txt" 2>&1; then
  echo 'Expected nonzero exit status for mock failed dependency' >&2
  exit 1
else
  status=$?
  [[ "$status" -eq 23 ]]
fi
[[ -f "$work/failure/deps-final-libdrm.log.gz" ]]
gzip -cd "$work/failure/deps-final-libdrm.log.gz" | grep -Fq 'configure: error: deliberately missing dependency'
grep -Fq 'Possible first actionable errors' "$work/failure-output.txt"
grep -Fq 'configure: error: deliberately missing dependency' "$work/failure-output.txt"
grep -Fq 'FAILED' "$GITHUB_STEP_SUMMARY"
echo 'PASS: staged dependency selection, compact successful logs, and failure reporting'
