#!/usr/bin/env bash
# Host-only: real sampler/ALSA callback code with simulated stream/PCM APIs.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
read -r -a cc_cmd <<< "${CC:-cc}"
read -r -a audio_cflags <<< "${DIAG_TEST_CFLAGS:-$(pkg-config --cflags alsa opus)}"
flags=(-std=c99 -Wall -Wextra -Werror -Wno-unused-parameter "-I$root/third_party/moonlight-common-c/src")
"${cc_cmd[@]}" "${flags[@]}" "$root/tests/test_stream_diagnostics.c" \
  "$root/src/osd/diagnostics.c" "$root/src/osd/stats.c" -o "$tmp/format"
"$tmp/format"
"${cc_cmd[@]}" "${flags[@]}" -DHAVE_DRM_OSD -DHAVE_ALSA -rdynamic \
  "$root/tests/test_stream_monitor.c" "$root/src/osd/osd.c" \
  "$root/src/osd/diagnostics.c" "$root/src/osd/stats.c" "$root/src/osd/vdec.c" \
  -pthread -ldl -o "$tmp/monitor"
"$tmp/monitor"
"${cc_cmd[@]}" "${flags[@]}" "${audio_cflags[@]}" -ffunction-sections -fdata-sections \
  "$root/tests/test_alsa_diagnostics.c" "$root/src/audio/alsa_resync.c" \
  -Wl,--gc-sections -o "$tmp/audio"
"$tmp/audio"
