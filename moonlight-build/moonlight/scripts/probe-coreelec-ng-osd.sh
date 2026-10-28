#!/bin/sh
# Read-only CoreELEC NG display capability probe. Never touch VFM mappings,
# framebuffer state, DRM master, or HEVC module parameters.
set -eu

root=${OSD_PROBE_ROOT:-/}
case "$root" in
  /*) ;;
  *) echo "OSD_PROBE_ROOT must be an absolute directory" >&2; exit 2 ;;
esac
root=${root%/}

node() {
  rel="$1"
  label="$2"
  path="$root$rel"
  if [ ! -e "$path" ]; then
    printf '%s: absent\n' "$label"
  elif [ -r "$path" ] && [ -f "$path" ]; then
    # Bounded output, no attempted sysfs writes. Safe even for virtual files.
    value="$(head -n 1 "$path" 2>/dev/null | cut -c 1-180 || true)"
    printf '%s: %s\n' "$label" "${value:-empty}"
  else
    printf '%s: present (not readable as regular text)\n' "$label"
  fi
}

printf '%s\n' 'CoreELEC NG OSD capability probe (read-only)'
node /dev/dri/card0 /dev/dri/card0
node /dev/fb0 /dev/fb0
node /dev/fb1 /dev/fb1
node /sys/class/graphics/fb0/name fb0-name
node /sys/class/graphics/fb1/name fb1-name
node /sys/class/graphics/fb0/blank fb0-blank
node /sys/class/graphics/fb1/blank fb1-blank
node /sys/module/amvideo/parameters/display_frame_count amvideo-display-frame-count
node /sys/class/video/vframe_states amvideo-vframe-states
node /sys/class/thermal/thermal_zone0/type thermal-zone0-type
node /sys/class/thermal/thermal_zone0/temp thermal-zone0-temperature
node /proc/stat aggregate-cpu-ticks
node /sys/class/vfm/map vfm-map

printf '%s\n' 'This does not test DRM master ownership, plane availability, or safe fb1 use.'
printf '%s\n' 'Do not write to /dev/fb1, /dev/dri/card0 or VFM as a result of this probe.'
