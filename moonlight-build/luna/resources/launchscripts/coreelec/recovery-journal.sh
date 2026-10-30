#!/bin/sh
# Restore session-scoped CPU/network tuning without losing retry information.
# Values changed by another owner are not overwritten.
# A recovery file is not permission to write arbitrary paths, even after
# process ownership has been verified. Accept only the knobs we journal.
luna_valid_tuning_target() {
  vt_kind="$1"
  vt_path="$2"
  vt_original="$3"
  vt_target="$4"
  case "$vt_kind" in
    network)
      vt_root="${LUNA_NET_CORE_ROOT:-/proc/sys/net/core}"
      case "$vt_path" in
        "$vt_root"/rmem_max|"$vt_root"/rmem_default|"$vt_root"/netdev_max_backlog) ;;
        *) return 1 ;;
      esac
      case "$vt_original:$vt_target" in *[!0-9:]*|:*|*:) return 1 ;; esac ;;
    governor)
      vt_root="${LUNA_CPUFREQ_ROOT:-/sys/devices/system/cpu/cpufreq}"
      case "$vt_path" in "$vt_root"/policy*/scaling_governor) ;; *) return 1 ;; esac
      vt_policy="${vt_path#"$vt_root"/policy}"
      vt_policy="${vt_policy%/scaling_governor}"
      case "$vt_policy" in ''|*[!0-9]*) return 1 ;; esac
      case "$vt_original" in ''|*[!A-Za-z0-9_-]*) return 1 ;; esac
      [ "$vt_target" = performance ] || return 1 ;;
    *) return 1 ;;
  esac
  return 0
}

restore_tuning_journal() {
  rj_file="$1"
  rj_kind="$2"
  [ -n "$rj_file" ] && [ -e "$rj_file" ] || return 0
  if [ ! -s "$rj_file" ]; then
    rm -f "$rj_file"
    return $?
  fi
  rj_pending="$(mktemp "$rj_file.pending.XXXXXXXX")" || return 1
  rj_fail=0
  rj_journal_error=0
  while :; do
    rj_path='' rj_original='' rj_target=''
    if ! IFS='|' read -r rj_path rj_original rj_target; then
      # A non-newline-terminated final entry is incomplete. POSIX read
      # populates its fields but returns failure, so detect and preserve it.
      if [ -n "$rj_path$rj_original$rj_target" ]; then
        echo 'Luna NG: truncated tuning recovery entry; retaining journal' >&2
        rj_journal_error=1
      fi
      break
    fi
    if [ -z "$rj_path" ] || [ -z "$rj_original" ]; then
      echo 'Luna NG: malformed tuning recovery entry' >&2
      rj_journal_error=1
      break
    fi
    if [ "$rj_kind" = governor ]; then
      # Governor journals have only two fields; reject a forged third.
      [ -z "$rj_target" ] || { rj_journal_error=1; break; }
      rj_target=performance
    elif [ "$rj_kind" != network ] || [ -z "$rj_target" ]; then
      echo 'Luna NG: invalid tuning recovery entry type' >&2
      rj_journal_error=1
      break
    fi
    if ! luna_valid_tuning_target "$rj_kind" "$rj_path" "$rj_original" "$rj_target"; then
      echo 'Luna NG: unsafe tuning recovery path or value' >&2
      rj_journal_error=1
      break
    fi
    rj_restore=0
    if [ ! -r "$rj_path" ] || [ ! -w "$rj_path" ]; then
      rj_restore=1
    else
      rj_current="$(cat "$rj_path" 2>/dev/null)" || rj_current=''
      if [ -z "$rj_current" ]; then
        rj_restore=1
      elif [ "$rj_current" = "$rj_original" ]; then
        : # Already restored.
      elif [ "$rj_current" != "$rj_target" ]; then
        echo "Luna NG: tuning changed externally; preserving $rj_path" >&2
      elif ! printf '%s' "$rj_original" > "$rj_path" 2>/dev/null; then
        rj_restore=1
      else
        rj_verified="$(cat "$rj_path" 2>/dev/null)" || rj_verified=''
        [ "$rj_verified" = "$rj_original" ] || rj_restore=1
      fi
    fi
    if [ "$rj_restore" -eq 1 ]; then
      echo "Luna NG: tuning restore deferred for $rj_path" >&2
      if [ "$rj_kind" = governor ]; then
        printf '%s|%s\n' "$rj_path" "$rj_original" >> "$rj_pending" || rj_journal_error=1
      else
        printf '%s|%s|%s\n' "$rj_path" "$rj_original" "$rj_target" >> "$rj_pending" || rj_journal_error=1
      fi
      rj_fail=1
    fi
    [ "$rj_journal_error" -eq 0 ] || break
  done < "$rj_file"
  if [ "$rj_journal_error" -ne 0 ]; then
    echo 'Luna NG: cannot persist tuning journal; retaining original' >&2
    rm -f "$rj_pending"
    return 1
  fi
  if [ "$rj_fail" -ne 0 ]; then
    if ! mv -f "$rj_pending" "$rj_file"; then
      rm -f "$rj_pending"
      return 1
    fi
    return 1
  fi
  rm -f "$rj_pending" "$rj_file"
}
