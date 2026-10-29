#!/bin/sh
# Shared AML state recovery. Restore only values still matching Moonlight tuning.
aml_snapshot() {
  : > "$1" || return 1
  for spec in "low|${LUNA_AML_LOW_LATENCY:-/sys/module/aml_media/parameters/use_low_latency}|1" "skip|${LUNA_AML_SKIP_POLICY:-/sys/module/amvdec_h265/parameters/nal_skip_policy}|0" "ref|${LUNA_AML_REF_FLAGS:-/sys/module/amvdec_h265/parameters/ref_frame_mark_flag}|0,0,0,0,0,0,0,0,0"; do
    kind="${spec%%|*}"; rest="${spec#*|}"; path="${rest%%|*}"; expected="${rest#*|}"
    [ -r "$path" ] && [ -w "$path" ] || continue
    original="$(cat "$path" 2>/dev/null)" || continue
    case "$kind:$original" in
      low:0|low:1|skip:2|skip:0|ref:1,1,1,1,1,1,1,1,1|ref:0,0,0,0,0,0,0,0,0) ;;
      *) continue ;;
    esac
    [ "$original" = "$expected" ] && continue
    printf "%s|%s|%s\n" "$path" "$original" "$expected" >> "$1" || return 1
  done
}
aml_restore() {
  [ -n "$1" ] && [ -f "$1" ] || return 0
  while IFS="|" read -r path original expected; do
    [ -n "$path" ] && [ -n "$original" ] && [ -n "$expected" ] || continue
    [ -r "$path" ] && [ -w "$path" ] || continue
    current="$(cat "$path" 2>/dev/null)" || continue
    [ "$current" = "$expected" ] || continue
    printf "%s" "$original" > "$path" 2>/dev/null || echo "AML restore failed: $path" >&2
  done < "$1"
  rm -f "$1"
}
aml_reset_video() {
  video="${LUNA_VIDEO_DISABLE:-/sys/class/video/disable_video}"
  status="${LUNA_VDEC_STATUS:-/sys/class/vdec/vdec_status}"
  if [ -e "$video" ] && [ -w "$video" ]; then printf 1 > "$video" 2>/dev/null || true; fi
  if [ -r "$status" ] && ! grep -Fq "No vdec." "$status" 2>/dev/null; then
    attempt=0
    while [ "$attempt" -lt 3 ]; do
      sleep 1
      grep -Fq "No vdec." "$status" 2>/dev/null && break
      attempt=$((attempt + 1))
    done
  fi
}
