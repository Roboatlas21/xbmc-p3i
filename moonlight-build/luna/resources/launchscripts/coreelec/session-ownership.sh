#!/bin/sh
# Verify a stopped prior systemd session before recover-only sysfs restoration.
# A dead launcher PID alone does NOT prove that Moonlight's cgroup has exited.

# The write/rename sequence is atomic on the session directory filesystem.
# Presence alone is never evidence of an exit; every receipt has exact content.
luna_write_receipt() {
  lr_path="$1"
  lr_value="$2"
  lr_tmp="$(mktemp "$lr_path.XXXXXXXX")" || return 1
  if printf '%s\n' "$lr_value" > "$lr_tmp" && mv -f "$lr_tmp" "$lr_path"; then
    return 0
  fi
  rm -f "$lr_tmp"
  return 1
}
luna_receipt_is() {
  [ -f "$1" ] && [ -r "$1" ] || return 1
  # A sentinel preserves trailing newlines during command substitution.
  # Accept exactly the expected value and one terminating newline.
  [ "$(cat "$1" 2>/dev/null && printf '#')" = "$(printf '%s\n#' "$2")" ]
}

# Filesystem updates can mark a still-running executable as " (deleted)".
# Match only the same originally recorded executable when terminating it.
luna_is_moonlight_executable() {
  case "$1" in */moonlight|*/moonlight\ \(deleted\)) return 0 ;; esac
  return 1
}
luna_native_executable_matches() {
  # Missing/corrupt recovery identities must never match an unreadable /proc exe.
  [ -n "$2" ] && luna_is_moonlight_executable "$2" || return 1
  [ "$1" = "$2" ] || [ "$1" = "$2 (deleted)" ]
}

# Confirm an exact Moonlight PID/start pair is gone or a zombie with no open
# resources. PID reuse does not count as a surviving old native process.
luna_recorded_native_gone() {
  ln_session="$1"
  [ -e "$ln_session.moonlight-pid" ] || return 0
  [ -r "$ln_session.moonlight-pid" ] || return 1
  IFS='|' read -r ln_pid ln_start ln_exe < "$ln_session.moonlight-pid" || return 1
  case "$ln_pid:$ln_start" in *[!0-9:]*|:*|*:) return 1 ;; esac
  [ -n "$ln_exe" ] || return 1
  if [ -r "/proc/$ln_pid/stat" ]; then
    ln_stat="$(sed 's/^.*) //' "/proc/$ln_pid/stat" 2>/dev/null)" || return 1
    ln_state="${ln_stat%% *}"
    ln_time="$(printf '%s\n' "$ln_stat" | awk '{print $20}')"
    if [ "$ln_time" = "$ln_start" ]; then
      case "$ln_state" in Z|X|x) : ;; *) return 1 ;; esac
    fi
  fi
  return 0
}

# Shared launcher/watchdog guard. Do not restore global decoder state if an
# exact recorded process or Moonlight process in the known cgroup still runs.
luna_native_processes_stopped() {
  ln_session="$1"
  ln_unit="$2"
  if [ -e "$ln_session.native-not-started" ]; then
    luna_receipt_is "$ln_session.native-not-started" not-started || return 1
    [ ! -e "$ln_session.native-launch-attempted" ] || return 1
  elif [ -e "$ln_session.native-exited" ]; then
    luna_receipt_is "$ln_session.native-exited" exited || return 1
  elif [ ! -r "$ln_session.moonlight-pid" ]; then
    return 1
  fi
  if [ -e "$ln_session.native-launch-attempted" ]; then
    luna_receipt_is "$ln_session.native-launch-attempted" attempted || return 1
  fi
  luna_recorded_native_gone "$ln_session" || return 1

  ln_group="$(timeout -k 2 5 "$SYSTEMCTL" show --property=ControlGroup --value "$ln_unit" 8>&- 9>&- 2>/dev/null)" || return 1
  case "$ln_group" in
    /) return 1 ;;
    '')
      # A collected unit supplies no process inventory. Only a positively
      # recorded exit/not-started state can establish the native boundary.
      luna_receipt_is "$ln_session.native-exited" exited ||
        luna_receipt_is "$ln_session.native-not-started" not-started ;;
    /*)
      for ln_proc in /proc/[0-9]*/cgroup; do
        [ -r "$ln_proc" ] || continue
        if awk -F: -v group="$ln_group" '$3 == group {found=1} END {exit !found}' "$ln_proc" 2>/dev/null; then
          ln_pid="${ln_proc#/proc/}"; ln_pid="${ln_pid%/cgroup}"
          ln_exe="$(readlink "/proc/$ln_pid/exe" 2>/dev/null || true)"
          luna_is_moonlight_executable "$ln_exe" && return 1
        fi
      done
      return 0 ;;
    *) return 1 ;;
  esac
}

luna_session_unit_quiescent() {
  us_unit="$1"
  us_session="$2"
  us_state="$(timeout -k 2 5 "$SYSTEMCTL" show --property=ActiveState --value "$us_unit" 8>&- 9>&- 2>/dev/null)" || return 1
  case "$us_state" in inactive|failed) ;; *) return 1 ;; esac

  # The original process must be absent or have a DIFFERENT start time.
  [ -r "$us_session.owner" ] || return 1
  IFS='|' read -r us_pid us_start < "$us_session.owner" || return 1
  case "$us_pid:$us_start" in *[!0-9:]*|:*|*:) return 1 ;; esac
  if [ -r "/proc/$us_pid/stat" ]; then
    us_now="$(sed 's/^.*) //' "/proc/$us_pid/stat" 2>/dev/null | awk '{print $20}')"
    [ "$us_now" != "$us_start" ] || return 1
  fi

  # Shared proof also validates receipt content and checks exact native PID.
  luna_native_processes_stopped "$us_session" "$us_unit" || return 1

  # A reported cgroup path is a second ownership boundary. Reject recovery
  # if anything remains in that exact control group, even if systemd reports
  # 'failed' or the recorded binary PID has disappeared.
  us_group="$(timeout -k 2 5 "$SYSTEMCTL" show --property=ControlGroup --value "$us_unit" 8>&- 9>&- 2>/dev/null)" || return 1
  case "$us_group" in
    '') return 0 ;;
    /) return 1 ;;
    /*)
      for us_proc in /proc/[0-9]*/cgroup; do
        [ -r "$us_proc" ] || continue
        if awk -F: -v group="$us_group" '$3 == group {found=1} END {exit !found}' "$us_proc" 2>/dev/null; then
          return 1
        fi
      done
      return 0 ;;
    *) return 1 ;;
  esac
}

# A shell pattern's * matches '/', including '../' traversals. Always
# validate the single basename BEFORE touching a file or its sibling.
luna_owned_file_valid() {
  lv_dir="$1"
  lv_path="$2"
  lv_kind="$3"
  case "$lv_dir" in /*) [ -d "$lv_dir" ] || return 1 ;; *) return 1 ;; esac
  case "$lv_path" in "$lv_dir"/*) ;; *) return 1 ;; esac
  lv_name="${lv_path#"$lv_dir"/}"
  case "$lv_name" in ''|*/*|*'..'*) return 1 ;; esac
  case "$lv_kind:$lv_name" in
    stream:.luna-stream-*.conf|ack:.luna-ack-*.ack|abandoned:.luna-ack-*.ack.abandoned) ;;
    *) return 1 ;;
  esac
  [ ! -L "$lv_path" ] || return 1
  [ ! -e "$lv_path" ] || [ -f "$lv_path" ] || return 1
  return 0
}

# Recovery records never authorize arbitrary paths. In shell patterns,
# even wildcards can match '/' and '../', so verify every basename.
luna_valid_session_marker() {
  sm_session="$1"
  sm_dir="${sm_session%/*}"
  case "$sm_dir" in /*) [ -d "$sm_dir" ] || return 1 ;; *) return 1 ;; esac
  case "$sm_dir" in */../*|*/./*|*/..|*/.) return 1 ;; esac
  case "$sm_session" in "$sm_dir"/.luna-session.????????) ;; *) return 1 ;; esac
  sm_name="${sm_session#"$sm_dir"/}"
  case "$sm_name" in ''|*/*|*'..'*) return 1 ;; esac
  [ ! -L "$sm_session" ] || return 1
  [ ! -e "$sm_session" ] || [ -f "$sm_session" ] || return 1
}
luna_valid_session_journal() {
  vj_session="$1"
  vj_path="$2"
  vj_kind="$3"
  luna_valid_session_marker "$vj_session" || return 1
  [ -n "$vj_path" ] || return 1
  vj_dir="${vj_session%/*}"
  case "$vj_path" in "$vj_dir"/*) ;; *) return 1 ;; esac
  vj_name="${vj_path#"$vj_dir"/}"
  case "$vj_name" in ''|*/*|*'..'*) return 1 ;; esac
  case "$vj_kind" in governor|network|tsync|hdmi|aml) ;; *) return 1 ;; esac
  if [ "$vj_path" != "$vj_session.$vj_kind-state" ]; then
    # Older builds used random directory-local names for these four journals.
    case "$vj_kind:$vj_path" in
      governor:"$vj_dir"/.luna-governor.????????|network:"$vj_dir"/.luna-network.????????|tsync:"$vj_dir"/.luna-tsync.????????|hdmi:"$vj_dir"/.luna-hdmi.????????) ;;
      *) return 1 ;;
    esac
  fi
  [ ! -L "$vj_path" ] || return 1
  [ ! -e "$vj_path" ] || [ -f "$vj_path" ] || return 1
}
luna_load_session_journals() {
  mj_session="$1"
  luna_valid_session_marker "$mj_session" || return 1
  mj_file="$mj_session.journals"
  [ -f "$mj_file" ] && [ ! -L "$mj_file" ] && [ -r "$mj_file" ] || return 1
  [ "$(wc -l < "$mj_file")" -eq 5 ] || return 1
  LUNA_JOURNAL_GOV="$(sed -n '1p' "$mj_file")" || return 1
  LUNA_JOURNAL_NET="$(sed -n '2p' "$mj_file")" || return 1
  LUNA_JOURNAL_TSYNC="$(sed -n '3p' "$mj_file")" || return 1
  LUNA_JOURNAL_HDMI="$(sed -n '4p' "$mj_file")" || return 1
  LUNA_JOURNAL_AML="$(sed -n '5p' "$mj_file")" || return 1
  luna_valid_session_journal "$mj_session" "$LUNA_JOURNAL_GOV" governor &&
  luna_valid_session_journal "$mj_session" "$LUNA_JOURNAL_NET" network &&
  luna_valid_session_journal "$mj_session" "$LUNA_JOURNAL_TSYNC" tsync &&
  luna_valid_session_journal "$mj_session" "$LUNA_JOURNAL_HDMI" hdmi &&
  luna_valid_session_journal "$mj_session" "$LUNA_JOURNAL_AML" aml
}

# Only retire files which this session explicitly recorded, and only after
# native/cgroup ownership was proved gone. Reject arbitrary journal paths.
luna_cleanup_owned_launch_files() {
  lc_session="$1"
  [ -f "$lc_session.launch-files" ] || return 0
  [ -r "$lc_session.launch-files" ] || return 1
  lc_dir="$(sed -n '1p' "$lc_session.launch-files")" || return 1
  lc_config="$(sed -n '2p' "$lc_session.launch-files")" || return 1
  lc_ack="$(sed -n '3p' "$lc_session.launch-files")" || return 1
  if [ -z "$lc_dir" ] && [ -z "$lc_config" ] && [ -z "$lc_ack" ]; then
    rm -f "$lc_session.launch-files"
    return $?
  fi
  case "$lc_dir" in /*) [ -d "$lc_dir" ] || return 1 ;; *) return 1 ;; esac
  # The launcher and watchdog pass their actual native binary directory.
  # Recovery metadata cannot redirect cleanup outside that trusted directory.
  lc_trusted="${LUNA_NATIVE_BIN_DIR:-${bin_dir:-}}"
  if [ -n "$lc_trusted" ] && [ "$lc_dir" != "$lc_trusted" ]; then
    return 1
  fi
  # Validate ALL entries before deleting ANY. A corrupt second entry must
  # never lead to half-completed cleanup of the first.
  [ -z "$lc_config" ] ||
    luna_owned_file_valid "$lc_dir" "$lc_config" stream || return 1
  [ -z "$lc_ack" ] ||
    luna_owned_file_valid "$lc_dir" "$lc_ack" ack || return 1
  [ -z "$lc_ack" ] ||
    luna_owned_file_valid "$lc_dir" "$lc_ack.abandoned" abandoned || return 1
  if [ -n "$lc_config" ]; then
    rm -f "$lc_config" || return 1
  fi
  if [ -n "$lc_ack" ]; then
    rm -f "$lc_ack.abandoned" "$lc_ack" || return 1
  fi
  rm -f "$lc_session.launch-files"
}

# Unarmed records may precede creation of .journals. Validate both
# session-derived paths and older mktemp journal names before deleting.
luna_valid_unarmed_journal() {
  [ "$2" = "${1%/*}" ] && luna_valid_session_journal "$1" "$3" "$4"
}

# A legacy launcher could be killed between mktemp and its first write.
# Such a marker is empty and has NO ownership metadata, native receipt, or
# recovery files. This retirement is deliberately narrower than the unarmed
# path; a non-empty or partially armed session is never treated as harmless.
luna_retire_empty_session() {
  re_session="$1"
  re_dir="$2"
  case "$re_session" in "$re_dir"/.luna-session.????????) ;; *) return 1 ;; esac
  [ -f "$re_session" ] && [ ! -L "$re_session" ] &&
    [ ! -s "$re_session" ] || return 1
  for re_suffix in native-not-started native-launch-attempted native-exited \
                   moonlight-pid kodi-initial startup display-pending \
                   journals journals.pending launch-files launch-files.pending \
                   owner unit boot-id ready stream-ready cleanup \
                   governor-state network-state tsync-state hdmi-state \
                   aml-state recovery.lock; do
    [ ! -e "$re_session.$re_suffix" ] &&
      [ ! -L "$re_session.$re_suffix" ] || return 1
  done
  rm -f "$re_session" || return 1
  echo "Luna NG: retired empty legacy pre-launch marker $re_session" >&2
}

# A zero-armed session cannot have launched Moonlight. Reclaim it only under
# the shared launch lock, and only if no old launcher/watchdog/unit survives.
# Incomplete or forged ownership metadata is never permission to delete.
luna_retire_unarmed_session() {
  ru_session="$1"
  ru_dir="$2"
  case "$ru_session" in "$ru_dir"/.luna-session.????????) ;; *) return 1 ;; esac
  luna_receipt_is "$ru_session" 0 || return 1
  if [ -e "$ru_session.native-not-started" ]; then
    luna_receipt_is "$ru_session.native-not-started" not-started || return 1
  else
    # The launcher first creates the zero-arm marker, then writes
    # native-not-started. If it died between those instructions, no
    # watchdog or native resource could have been launched.
    for ru_marker in "$ru_session.journals" "$ru_session.owner" \
                     "$ru_session.unit" "$ru_session.ready" \
                     "$ru_session.boot-id" "$ru_session.launch-files" \
                     "$ru_session.journals.pending" \
                     "$ru_session.launch-files.pending"; do
      [ ! -e "$ru_marker" ] || return 1
    done
  fi
  for ru_marker in "$ru_session.native-launch-attempted" \
                   "$ru_session.native-exited" "$ru_session.moonlight-pid" \
                   "$ru_session.kodi-initial" "$ru_session.startup" \
                   "$ru_session.display-pending"; do
    [ ! -e "$ru_marker" ] || return 1
  done

  if [ -e "$ru_session.owner" ] || [ -L "$ru_session.owner" ] ||
     [ -e "$ru_session.unit" ] || [ -L "$ru_session.unit" ]; then
    # Ownership is recorded immediately before the watchdog is submitted.
    # A kill after .owner but before .unit cannot have started the watchdog
    # or native process. Accept that narrow pre-watchdog case only while the
    # exact not-started receipt and all earlier metadata are intact.
    if [ -e "$ru_session.owner" ] && [ ! -e "$ru_session.unit" ] &&
       [ ! -L "$ru_session.unit" ]; then
      [ -f "$ru_session.owner" ] && [ ! -L "$ru_session.owner" ] &&
        [ -r "$ru_session.owner" ] || return 1
      [ -f "$ru_session.journals" ] && [ ! -L "$ru_session.journals" ] &&
        [ -f "$ru_session.launch-files" ] && [ ! -L "$ru_session.launch-files" ] &&
        [ -s "$ru_session.boot-id" ] && [ ! -L "$ru_session.boot-id" ] &&
        [ ! -e "$ru_session.ready" ] && [ ! -L "$ru_session.ready" ] &&
        luna_receipt_is "$ru_session.native-not-started" not-started || return 1
      [ "$(wc -l < "$ru_session.owner")" -eq 1 ] &&
        [ "$(wc -c < "$ru_session.owner")" -le 128 ] || return 1
      IFS='|' read -r ru_owner ru_started < "$ru_session.owner" || return 1
      case "$ru_owner:$ru_started" in *[!0-9:]*|:*|*:) return 1 ;; esac
      # Even a valid-looking PID receipt must not retire a live launcher.
      if [ -r "/proc/$ru_owner/stat" ]; then
        ru_now="$(sed 's/^.*) //' "/proc/$ru_owner/stat" 2>/dev/null |
            awk '{print $20}')" || return 1
        [ "$ru_now" != "$ru_started" ] || return 1
      fi
    else
      [ -r "$ru_session.owner" ] && [ ! -L "$ru_session.owner" ] &&
        [ -r "$ru_session.unit" ] && [ ! -L "$ru_session.unit" ] || return 1
      ru_unit="$(cat "$ru_session.unit" 2>/dev/null)" || return 1
      case "$ru_unit" in luna-ng-session-*.service) ;; *) return 1 ;; esac
      luna_session_unit_quiescent "$ru_unit" "$ru_session" || return 1
      IFS='|' read -r ru_owner ru_started < "$ru_session.owner" || return 1
      case "$ru_owner:$ru_started" in *[!0-9:]*|:*|*:) return 1 ;; esac
      ru_watchdog_state="$(timeout -k 2 5 "$SYSTEMCTL" show \
          --property=ActiveState --value "luna-ng-watchdog-$ru_owner.service" \
          8>&- 9>&- 2>/dev/null)" || return 1
      case "$ru_watchdog_state" in inactive|failed) ;; *) return 1 ;; esac
    fi
  else
    # Watchdog starts only after owner+unit metadata is written; without
    # either, it must not have been submitted. The caller holds launch flock.
    [ ! -e "$ru_session.ready" ] || return 1
  fi

  # The launch-file manifest is now renamed atomically. An abandoned
  # staging file is safe to remove only before service ownership metadata
  # exists and while the native-not-started receipt is still valid.
  if [ -e "$ru_session.launch-files.pending" ]; then
    [ -f "$ru_session.launch-files.pending" ] &&
      [ ! -L "$ru_session.launch-files.pending" ] &&
      [ ! -e "$ru_session.ready" ] &&
      [ ! -e "$ru_session.owner" ] && [ ! -e "$ru_session.unit" ] &&
      [ ! -e "$ru_session.boot-id" ] &&
      luna_receipt_is "$ru_session.native-not-started" not-started || return 1
  fi
  # Older builds wrote the final manifest directly and could be killed
  # midway. Never parse or follow content from a torn pre-watchdog record.
  ru_torn_launch=0
  if [ -e "$ru_session.launch-files" ]; then
    [ -f "$ru_session.launch-files" ] &&
      [ ! -L "$ru_session.launch-files" ] || return 1
    ru_launch_lines="$(wc -l < "$ru_session.launch-files")" || return 1
    ru_launch_bytes="$(wc -c < "$ru_session.launch-files")" || return 1
    if [ "$ru_launch_lines" -lt 3 ]; then
      [ "$ru_launch_bytes" -le 4096 ] &&
        [ ! -e "$ru_session.ready" ] &&
        [ ! -e "$ru_session.owner" ] && [ ! -e "$ru_session.unit" ] &&
        [ ! -e "$ru_session.boot-id" ] &&
        luna_receipt_is "$ru_session.native-not-started" not-started || return 1
      ru_torn_launch=1
    fi
  fi

  ru_gov='' ru_net='' ru_tsync='' ru_hdmi='' ru_aml=''
  if [ -e "$ru_session.journals.pending" ]; then
    # The new writer uses a private staging file before atomically renaming.
    # Cleanup is safe only before owner/watchdog/Kodi metadata appears.
    [ -f "$ru_session.journals.pending" ] &&
      [ ! -L "$ru_session.journals.pending" ] &&
      [ ! -e "$ru_session.ready" ] && [ ! -e "$ru_session.launch-files" ] &&
      [ ! -e "$ru_session.owner" ] && [ ! -e "$ru_session.unit" ] &&
      [ ! -e "$ru_session.boot-id" ] &&
      luna_receipt_is "$ru_session.native-not-started" not-started || return 1
  fi
  if [ -e "$ru_session.journals" ]; then
    if luna_load_session_journals "$ru_session"; then
      ru_gov="$LUNA_JOURNAL_GOV"
      ru_net="$LUNA_JOURNAL_NET"
      ru_tsync="$LUNA_JOURNAL_TSYNC"
      ru_hdmi="$LUNA_JOURNAL_HDMI"
      ru_aml="$LUNA_JOURNAL_AML"
    else
      # Legacy writer crashed during the final manifest write. Do not
      # interpret any of its paths or retire a session after watchdog start.
      [ -f "$ru_session.journals" ] && [ ! -L "$ru_session.journals" ] &&
        [ ! -e "$ru_session.ready" ] && [ ! -e "$ru_session.launch-files" ] &&
        [ ! -e "$ru_session.owner" ] && [ ! -e "$ru_session.unit" ] &&
        [ ! -e "$ru_session.boot-id" ] &&
        luna_receipt_is "$ru_session.native-not-started" not-started || return 1
      ru_lines="$(wc -l < "$ru_session.journals")" || return 1
      ru_bytes="$(wc -c < "$ru_session.journals")" || return 1
      [ "$ru_lines" -lt 5 ] && [ "$ru_bytes" -le 4096 ] || return 1
    fi
  fi

  if [ "$ru_torn_launch" -eq 0 ]; then
    luna_cleanup_owned_launch_files "$ru_session" || return 1
  fi
  if [ -n "$ru_gov" ]; then
    rm -f "$ru_gov" "$ru_net" "$ru_tsync" "$ru_hdmi" "$ru_aml" || return 1
  fi
  # A crash during scratch creation predates the complete journal manifest.
  # Those deterministic names may be present without any .journals entry.
  rm -f "$ru_session.governor-state" "$ru_session.network-state" \
        "$ru_session.tsync-state" "$ru_session.hdmi-state" \
        "$ru_session.aml-state" || return 1
  rm -f "$ru_session" "$ru_session.native-not-started" \
        "$ru_session.ready" "$ru_session.stream-ready" \
        "$ru_session.journals" "$ru_session.journals.pending" "$ru_session.boot-id" \
        "$ru_session.owner" "$ru_session.unit" \
        "$ru_session.cleanup" "$ru_session.launch-files" \
        "$ru_session.launch-files.pending" || return 1
  echo "Luna NG: safely retired stale unarmed session $ru_session" >&2
  return 0
}


# A completed recovery also writes 0 to the session marker just before
# removing the remaining metadata. If it dies in that tiny window, the marker
# is NOT a never-started session. Distinguish it by positive native exit (or
# never-started) and fully restored journals, then recheck systemd/cgroup and
# acquire the old recovery lock before cleaning anything. The caller holds
# the stable launch flock; on uncertainty preserve all recovery evidence.
luna_retire_completed_session() {
  cs_session="$1"
  cs_dir="$2"
  case "$cs_session" in "$cs_dir"/.luna-session.????????) ;; *) return 1 ;; esac
  luna_valid_session_marker "$cs_session" &&
    luna_receipt_is "$cs_session" 0 || return 1
  [ ! -L "$cs_session.kodi-initial" ] &&
    [ ! -L "$cs_session.native-not-started" ] &&
    [ ! -L "$cs_session.native-launch-attempted" ] &&
    [ ! -L "$cs_session.native-exited" ] &&
    [ ! -L "$cs_session.moonlight-pid" ] &&
    [ ! -L "$cs_session.launch-files" ] || return 1
  [ -f "$cs_session.owner" ] && [ ! -L "$cs_session.owner" ] &&
    [ -f "$cs_session.unit" ] && [ ! -L "$cs_session.unit" ] &&
    [ -f "$cs_session.boot-id" ] && [ ! -L "$cs_session.boot-id" ] &&
    [ -s "$cs_session.boot-id" ] || return 1
  { luna_receipt_is "$cs_session.kodi-initial" active ||
    luna_receipt_is "$cs_session.kodi-initial" inactive; } || return 1

  if [ -e "$cs_session.native-launch-attempted" ] ||
     [ -L "$cs_session.native-launch-attempted" ]; then
    luna_receipt_is "$cs_session.native-launch-attempted" attempted &&
      luna_receipt_is "$cs_session.native-exited" exited &&
      [ ! -e "$cs_session.native-not-started" ] &&
      [ ! -L "$cs_session.native-not-started" ] || return 1
  else
    luna_receipt_is "$cs_session.native-not-started" not-started &&
      [ ! -e "$cs_session.native-exited" ] &&
      [ ! -L "$cs_session.native-exited" ] &&
      [ ! -e "$cs_session.moonlight-pid" ] &&
      [ ! -L "$cs_session.moonlight-pid" ] || return 1
  fi

  [ ! -e "$cs_session.display-pending" ] &&
    [ ! -L "$cs_session.display-pending" ] || return 1
  luna_load_session_journals "$cs_session" || return 1
  cs_gov="$LUNA_JOURNAL_GOV"
  cs_net="$LUNA_JOURNAL_NET"
  cs_tsync="$LUNA_JOURNAL_TSYNC"
  cs_hdmi="$LUNA_JOURNAL_HDMI"
  cs_aml="$LUNA_JOURNAL_AML"
  # Recovery cannot be considered completed while a nonempty journal still
  # requires a system setting to be restored. Reject symlinks even if empty.
  for cs_journal in "$cs_gov" "$cs_net" "$cs_tsync" "$cs_hdmi" "$cs_aml"; do
    [ ! -s "$cs_journal" ] && [ ! -L "$cs_journal" ] || return 1
  done

  cs_unit="$(cat "$cs_session.unit" 2>/dev/null)" || return 1
  case "$cs_unit" in luna-ng-session.service|luna-ng-session-*.service) ;;
    *) return 1 ;;
  esac
  case "$cs_unit" in *[!A-Za-z0-9_.-]*) return 1 ;; esac
  [ "$(wc -l < "$cs_session.owner")" -eq 1 ] &&
    [ "$(wc -c < "$cs_session.owner")" -le 128 ] || return 1
  IFS='|' read -r cs_pid cs_started < "$cs_session.owner" || return 1
  case "$cs_pid:$cs_started" in *[!0-9:]*|:*|*:) return 1 ;; esac
  cs_watchdog="$(timeout -k 2 5 "$SYSTEMCTL" show \
      --property=ActiveState --value "luna-ng-watchdog-$cs_pid.service" \
      8>&- 9>&- 2>/dev/null)" || return 1
  case "$cs_watchdog" in inactive|failed) ;; *) return 1 ;; esac
  [ -f "$cs_session.recovery.lock" ] &&
    [ ! -L "$cs_session.recovery.lock" ] || return 1

  # Acquire an independent old-session lock; do not race its watchdog or
  # normal cleanup. Only after exact native/unit/cgroup quiescence and safe
  # private-path cleanup do we remove the marker FIRST. An interruption in
  # later sidecar deletion therefore cannot strand the next launch again.
  (
    exec 9<>"$cs_session.recovery.lock" || exit 1
    flock -n 9 || exit 1
    luna_session_unit_quiescent "$cs_unit" "$cs_session" || exit 1
    luna_cleanup_owned_launch_files "$cs_session" || exit 1
    rm -f "$cs_session" || exit 1
    rm -f "$cs_session.ready" "$cs_session.stream-ready" \
          "$cs_session.cleanup" "$cs_session.startup" \
          "$cs_session.moonlight-pid" "$cs_session.display-pending" \
          "$cs_session.native-not-started" \
          "$cs_session.native-launch-attempted" \
          "$cs_session.native-exited" "$cs_session.kodi-initial" \
          "$cs_session.journals" "$cs_session.journals.pending" \
          "$cs_session.launch-files" "$cs_session.launch-files.pending" \
          "$cs_session.boot-id" "$cs_session.owner" "$cs_session.unit" \
          "$cs_gov" "$cs_net" "$cs_tsync" "$cs_hdmi" "$cs_aml" || exit 1
  ) || return 1
  echo "Luna NG: retired safely completed prior session $cs_session" >&2
}


# Reclaim only long-abandoned recovery lock inodes. Never unlink from a
# live session or while another process owns the flock: doing so creates
# two independent lock inodes and defeats recovery serialization.
# A one-hour grace period exceeds all watchdog lock waits and prevents a
# late but already queued recovery from racing deletion.
luna_gc_retired_locks() {
  gc_dir="$1"
  gc_now="$(date +%s)" || return 0
  case "$gc_now" in ''|*[!0-9]*) return 0 ;; esac
  for gc_file in "$gc_dir"/.luna-session.????????.recovery.lock; do
    [ -f "$gc_file" ] && [ ! -L "$gc_file" ] || continue
    gc_session="${gc_file%.recovery.lock}"
    [ ! -e "$gc_session" ] && [ ! -L "$gc_session" ] || continue
    gc_modified="$(stat -c %Y "$gc_file" 2>/dev/null)" || continue
    case "$gc_modified" in ''|*[!0-9]*) continue ;; esac
    [ "$gc_now" -ge "$gc_modified" ] || continue
    [ "$((gc_now - gc_modified))" -ge 3600 ] || continue
    # A companion ownership marker could mean a prior cleanup is incomplete.
    gc_safe=1
    for gc_tail in ready owner unit boot-id startup cleanup \
                   native-not-started native-launch-attempted native-exited \
                   moonlight-pid journals journals.pending launch-files \
                   launch-files.pending display-pending stream-ready \
                   governor-state network-state tsync-state hdmi-state \
                   aml-state; do
      if [ -e "$gc_session.$gc_tail" ] || [ -L "$gc_session.$gc_tail" ]; then
        gc_safe=0
        break
      fi
    done
    [ "$gc_safe" -eq 1 ] || continue
    (
      exec 9<>"$gc_file" || exit 0
      flock -n 9 || exit 0
      [ ! -e "$gc_session" ] && [ ! -L "$gc_session" ] || exit 0
      rm -f "$gc_file"
    ) || true
  done
  # A killed initial marker publish may leave harmless staging files; it
  # cannot affect recovery, but reclaim old non-symlinked stages as well.
  for gc_file in "$gc_dir"/.luna-initial.????????; do
    [ -f "$gc_file" ] && [ ! -L "$gc_file" ] || continue
    gc_modified="$(stat -c %Y "$gc_file" 2>/dev/null)" || continue
    case "$gc_modified" in ''|*[!0-9]*) continue ;; esac
    [ "$gc_now" -ge "$gc_modified" ] || continue
    [ "$((gc_now - gc_modified))" -ge 3600 ] || continue
    gc_session="$gc_dir/.luna-session.${gc_file##*.}"
    [ ! -e "$gc_session" ] && [ ! -L "$gc_session" ] || continue
    rm -f "$gc_file" || true
  done
}
