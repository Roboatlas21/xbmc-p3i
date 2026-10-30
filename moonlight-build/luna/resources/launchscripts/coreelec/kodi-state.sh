#!/bin/sh
# Returns a conservative service state, not the ambiguous exit code of is-active.
# Unknown, activating, and deactivating never authorize Kodi-owned display writes.
kodi_service_state() {
  ks_raw="$(timeout -k 2 5 "$SYSTEMCTL" show --property=ActiveState --value "$KODI_UNIT" 8>&- 9>&- 2>/dev/null)" || {
    printf '%s\n' unknown
    return 0
  }
  case "$ks_raw" in
    active) printf '%s\n' active ;;
    inactive|failed) printf '%s\n' inactive ;;
    *) printf '%s\n' unknown ;;
  esac
}
