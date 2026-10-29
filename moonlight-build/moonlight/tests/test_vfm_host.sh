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
# Compile and exercise the real callback, event-loop and bounded-write code.
for test in test_aml_write_progress test_connection_callbacks test_loop_fd test_evdev_stable_slots; do
  "${cc_cmd[@]}" "${flags[@]}" "$root/tests/$test.c" -pthread -o "$tmp/$test"
  "$tmp/$test"
done
"${cc_cmd[@]}" "${flags[@]}" -c "$root/tests/test_mlvfm_abi.c" -o "$tmp/abi.o"
for test in test_aml_vfm_frame test_vfm_worker; do
  "${cc_cmd[@]}" "${flags[@]}" "${ff_cflags[@]}" "$root/tests/$test.c" \
    "$root/src/video/aml_vfm_frame.c" "${ff_libs[@]}" -pthread -o "$tmp/$test"
  "$tmp/$test"
done
bash "$root/tests/test_vfm_selection.sh"

# The production evdev implementation must survive an async rumble/unplug race.
"${cc_cmd[@]}" -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
  -Wno-unused-function -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "-I$root/third_party/moonlight-common-c/src" \
  $(pkg-config --cflags libevdev libudev) \
  "$root/tests/test_evdev_rumble_lifetime.c" \
  $(pkg-config --libs libevdev libudev) -Wl,--gc-sections \
  -Wl,--wrap=ioctl -pthread -lm -o "$tmp/rumble-lifetime"
ASAN_OPTIONS=detect_leaks=1 timeout -k 2 15 "$tmp/rumble-lifetime"
# Both native and Luna's Python XML paths must validate app entries.
bash "$root/tests/test_native_xml.sh"
