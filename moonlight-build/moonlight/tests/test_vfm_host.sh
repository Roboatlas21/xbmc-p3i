#!/usr/bin/env bash
# Fast device-free tests; uses host FFmpeg/libdrm development headers/libraries.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
read -r -a cc_cmd <<< "${CC:-cc}"
read -r -a ff_cflags <<< "${VFM_TEST_CFLAGS:-$(pkg-config --cflags libavcodec libavutil libdrm)}"
read -r -a ff_libs <<< "${VFM_TEST_LIBS:-$(pkg-config --libs libavcodec libavutil)}"
flags=(-std=c99 -Wall -Wextra -Werror -Wno-unused-parameter
       "-I$root/third_party/moonlight-common-c/src")
"${cc_cmd[@]}" "${flags[@]}" "$root/tests/test_mlvfm_queue.c" -o "$tmp/queue"
"$tmp/queue"
"${cc_cmd[@]}" "${flags[@]}" -c "$root/tests/test_mlvfm_abi.c" -o "$tmp/abi.o"
for test in test_aml_vfm_frame test_vfm_worker; do
  "${cc_cmd[@]}" "${flags[@]}" "${ff_cflags[@]}" "$root/tests/$test.c" \
    "$root/src/video/aml_vfm_frame.c" "${ff_libs[@]}" -pthread -o "$tmp/$test"
  "$tmp/$test"
done
bash "$root/tests/test_vfm_selection.sh"
