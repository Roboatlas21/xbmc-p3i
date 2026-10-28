#!/usr/bin/env bash
# Check native source preparation and logging with a mock CoreELEC package tree.
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
bash "$runner" vfm "$ce" "$work/logs" >> "$work/out.txt"
bash "$runner" moonlight "$ce" "$work/logs" >> "$work/out.txt"
cat > "$work/expected.txt" <<'EXPECTED'
toolchain
curl
pulseaudio
libamcodec
libdrm
moonlight-vfm-ffmpeg
moonlight-vfm-driver
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

# Exercise the real recipe through the pinned CoreELEC scripts/extract directory
# copy. Git objects are read-only, including when actions/checkout puts a nested
# repository inside the source tree. Recopying them in pre_build_target used to
# abort the native build before the compiler ran.
check_source_unpack() (
  ROOT="$work/source-$1/CoreELEC"
  SYSROOT_PREFIX="$ROOT/sysroot"
  MOONLIGHT_BUILD_VFM="$1"
  unset PKG_TAR_COPY_OPTS
  source "$here/packaging/coreelec-ng/package.mk"
  source_tree="${PKG_URL#file://}"
  common="$source_tree/third_party/moonlight-common-c"
  controller_db="$ROOT/controller-db"
  mkdir -p "$common/src" "$common/nanors" "$common/enet/include/enet" \
    "$common/.git/objects/pack" "$source_tree/.git/objects/pack" \
    "$common/nanors/.git/objects/pack" "$controller_db"
  printf 'pinned Limelight header\n' > "$common/src/Limelight.h"
  printf 'pinned nanoRS source\n' > "$common/nanors/rs.c"
  printf 'pinned Moonlight ENet header\n' > "$common/enet/include/enet/enet.h"
  printf '#!/bin/sh\nexit 0\n' > "$common/enet/configure"
  chmod +x "$common/enet/configure"
  ln -s Limelight.h "$common/src/header-link"
  printf 'keep ignore rules\n' > "$common/.gitignore"
  printf 'gitdir: ../.git/modules/enet\n' > "$common/enet/.git"
  for repo in "$source_tree" "$common" "$common/nanors"; do
    printf 'read-only Git object\n' > "$repo/.git/objects/pack/fixture.pack"
    chmod 444 "$repo/.git/objects/pack/fixture.pack"
  done
  printf 'controller mappings\n' > "$controller_db/gamecontrollerdb.txt"
  PKG_BUILD="$ROOT/build/$PKG_NAME-$PKG_VERSION"
  mkdir -p "$PKG_BUILD"
  # Match CoreELEC's intentional word splitting of PKG_TAR_COPY_OPTS.
  tar cf - -C "$source_tree" ${PKG_TAR_COPY_OPTS:-} . | tar xf - -C "$PKG_BUILD"
  get_build_dir() {
    [[ "$1" == sdlgamecontrollerdb ]]
    printf '%s\n' "$controller_db"
  }
  pre_build_target
  pre_build_target  # Re-entering the hook must also be safe.
  for file in src/Limelight.h nanors/rs.c enet/include/enet/enet.h .gitignore; do
    cmp "$common/$file" "$PKG_BUILD/third_party/moonlight-common-c/$file"
  done
  [[ -x "$PKG_BUILD/third_party/moonlight-common-c/enet/configure" ]]
  [[ "$(readlink "$PKG_BUILD/third_party/moonlight-common-c/src/header-link")" == Limelight.h ]]
  [[ -z "$(find "$PKG_BUILD" -name .git -print -quit)" ]]
  cmp "$controller_db/gamecontrollerdb.txt" \
    "$PKG_BUILD/third_party/SDL_GameControllerDB/gamecontrollerdb.txt"
  # Validate the extracted build sources, not merely the original checkout.
  rm "$PKG_BUILD/third_party/moonlight-common-c/enet/include/enet/enet.h"
  if pre_build_target > "$ROOT/missing-submodule.log" 2>&1; then
    echo 'Expected incomplete unpacked ENet sources to be rejected' >&2
    exit 1
  fi
  grep -Fq 'Missing moonlight-common-c/nanoRS/ENet' "$ROOT/missing-submodule.log"
)
check_source_unpack no
check_source_unpack yes
echo 'PASS: AML/VFM source unpack excludes Git metadata and preserves pinned submodules'
