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
# A journal may have been altered after capture. Validate both the path
# and its paired values before touching a sysfs node.
luna_valid_aml_recovery_target() {
  ar_path="$1"
  ar_original="$2"
  ar_expected="$3"
  if [ "$ar_path" = "${LUNA_AML_LOW_LATENCY:-/sys/module/aml_media/parameters/use_low_latency}" ]; then
    [ "$ar_expected" = 1 ] || return 1
    case "$ar_original" in 0|1) return 0 ;; esac
  elif [ "$ar_path" = "${LUNA_AML_SKIP_POLICY:-/sys/module/amvdec_h265/parameters/nal_skip_policy}" ]; then
    [ "$ar_expected" = 0 ] || return 1
    case "$ar_original" in 0|2) return 0 ;; esac
  elif [ "$ar_path" = "${LUNA_AML_REF_FLAGS:-/sys/module/amvdec_h265/parameters/ref_frame_mark_flag}" ]; then
    [ "$ar_expected" = 0,0,0,0,0,0,0,0,0 ] || return 1
    case "$ar_original" in 0,0,0,0,0,0,0,0,0|1,1,1,1,1,1,1,1,1) return 0 ;; esac
  fi
  return 1
}
aml_restore() {
  [ -n "$1" ] && [ -f "$1" ] || return 0
  # Keep the original snapshot until every pending record has been written.
  pending="$(mktemp "$1.pending.XXXXXXXX")" || return 1
  chmod 600 "$pending" 2>/dev/null || true
  failures=0
  journal_error=0
  while :; do
    path='' original='' expected=''
    if ! IFS='|' read -r path original expected; then
      # An incomplete last line may look like a valid entry, but read fails
      # without the terminating newline. Never discard that recovery record.
      if [ -n "$path$original$expected" ]; then
        echo 'AML restore: truncated journal record; preserving snapshot' >&2
        journal_error=1
      fi
      break
    fi
    if [ -z "$path" ] || [ -z "$original" ] || [ -z "$expected" ]; then
      echo 'AML restore: malformed journal record; preserving snapshot' >&2
      journal_error=1
      break
    fi
    case "$path$original$expected" in
      *'|'*) echo 'AML restore: extra journal fields; preserving snapshot' >&2
             journal_error=1; break ;;
    esac
    if ! luna_valid_aml_recovery_target "$path" "$original" "$expected"; then
      echo 'AML restore: unsafe recovery path or value; preserving snapshot' >&2
      journal_error=1
      break
    fi
    if [ ! -r "$path" ] || [ ! -w "$path" ]; then
      echo "AML restore deferred (unavailable): $path" >&2
      if ! printf '%s|%s|%s\n' "$path" "$original" "$expected" >> "$pending"; then
        journal_error=1
        break
      fi
      failures=1
      continue
    fi
    current="$(cat "$path" 2>/dev/null)" || {
      echo "AML restore deferred (unreadable): $path" >&2
      if ! printf '%s|%s|%s\n' "$path" "$original" "$expected" >> "$pending"; then
        journal_error=1
        break
      fi
      failures=1
      continue
    }
    [ "$current" = "$original" ] && continue
    if [ "$current" != "$expected" ]; then
      echo "AML restore: $path changed externally; not overwriting" >&2
      continue
    fi
    if ! printf '%s' "$original" > "$path" 2>/dev/null ||
       [ "$(cat "$path" 2>/dev/null)" != "$original" ]; then
      echo "AML restore deferred (write/readback failed): $path" >&2
      if ! printf '%s|%s|%s\n' "$path" "$original" "$expected" >> "$pending"; then
        journal_error=1
        break
      fi
      failures=1
    fi
  done < "$1"
  if [ "$journal_error" -ne 0 ]; then
    echo "AML restore journal write failed; preserving original recovery snapshot" >&2
    rm -f "$pending"
    return 1
  fi
  if [ "$failures" -ne 0 ]; then
    if ! mv -f "$pending" "$1"; then
      echo "AML restore journal replacement failed; preserving original" >&2
      rm -f "$pending"
      return 1
    fi
    return 1
  fi
  rm -f "$pending" "$1"
  return 0
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
