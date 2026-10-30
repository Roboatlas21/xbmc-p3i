#!/bin/sh
# Returns a conservative service state, not the ambiguous exit code of is-active.
# Unknown, activating, and deactivating never authorize Kodi-owned display writes.
# CoreELEC's BusyBox configuration omits timeout. Python is already required
# by Luna; keep command deadlines independent of optional host utilities.
luna_timeout() {
  python3 "$scripts/command-timeout.py" "$@"
}
kodi_service_state() {
  ks_raw="$(luna_timeout -k 2 5 "$SYSTEMCTL" show --property=ActiveState --value "$KODI_UNIT" 8>&- 9>&-)" || {
    ks_status=$?
    echo "Luna NG: service-state query for $KODI_UNIT failed (exit $ks_status)" >&2
    printf '%s\n' unknown
    return 0
  }
  case "$ks_raw" in
    active) printf '%s\n' active ;;
    inactive|failed) printf '%s\n' inactive ;;
    *) printf 'Luna NG: unexpected service state for %s: %.160s\n' "$KODI_UNIT" "$ks_raw" >&2
       printf '%s\n' unknown ;;
  esac
}
