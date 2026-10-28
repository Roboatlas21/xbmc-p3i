#!/bin/bash
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
script="$repo/scripts/probe-coreelec-ng-osd.sh"
bash -n "$script"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/sys/class/graphics/fb0" "$scratch/sys/class/graphics/fb1" \
         "$scratch/sys/class/video" "$scratch/sys/class/vfm" \
         "$scratch/sys/module/amvideo/parameters" "$scratch/dev"
printf 'amvideo\n' > "$scratch/sys/class/graphics/fb0/name"
printf '100\n' > "$scratch/sys/module/amvideo/parameters/display_frame_count"
printf 'vframe_pool_size=8\nvframe buf_avail_num=3\n' > "$scratch/sys/class/video/vframe_states"
printf 'decoder amvideo\n' > "$scratch/sys/class/vfm/map"
before="$(sha256sum "$scratch/sys/class/vfm/map" "$scratch/sys/class/graphics/fb0/name" \
  "$scratch/sys/module/amvideo/parameters/display_frame_count")"
OSD_PROBE_ROOT="$scratch" sh "$script" > "$scratch/result"
after="$(sha256sum "$scratch/sys/class/vfm/map" "$scratch/sys/class/graphics/fb0/name" \
  "$scratch/sys/module/amvideo/parameters/display_frame_count")"
[[ "$before" == "$after" ]]
grep -Fq 'amvideo-display-frame-count: 100' "$scratch/result"
grep -Fq '/dev/dri/card0: absent' "$scratch/result"
grep -Fq 'vfm-map: decoder amvideo' "$scratch/result"
if OSD_PROBE_ROOT=relative sh "$script" >/dev/null 2>&1; then
  echo 'Relative probe root should fail' >&2
  exit 1
fi
echo 'Read-only CoreELEC NG OSD probe: PASS'
