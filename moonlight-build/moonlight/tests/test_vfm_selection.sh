#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
flags=(-std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-switch
       "-I$root/third_party/moonlight-common-c/src")
cat > "$tmp/fake.c" <<'EOF'
#include <Limelight.h>
DECODER_RENDERER_CALLBACKS decoder_callbacks_aml_vfm = {0};
int moonlight_vfm_init(void (*stop)(void)) { (void)stop; return 0; }
EOF
cc "${flags[@]}" -DHAVE_AML_VFM "$root/tests/test_vfm_selection.c" \
  "$root/src/platform.c" -ldl -o "$tmp/selection"
LD_LIBRARY_PATH="$tmp" "$tmp/selection"
cc "${flags[@]}" -shared -fPIC "$tmp/fake.c" -o "$tmp/libmoonlight-aml-vfm.so"
LD_LIBRARY_PATH="$tmp" "$tmp/selection" available
cc "${flags[@]}" "$root/tests/test_vfm_selection.c" "$root/src/platform.c" \
  -ldl -o "$tmp/disabled"
LD_LIBRARY_PATH="$tmp" "$tmp/disabled"
echo 'PASS: explicit selection, missing plugin, and disabled build'
