#!/usr/bin/env bash
# Replace ONLY Pannal CoreELEC 21's dead rtmpdump URL, preserving its SHA256.
# Usage: fix-rtmpdump-source.sh /path/to/CoreELEC [--fetch]
set -euo pipefail

if (( $# < 1 || $# > 2 )) || [[ "${2:---patch}" != "--patch" && "${2:---patch}" != "--fetch" ]]; then
  echo "Usage: $0 /path/to/CoreELEC [--fetch]" >&2
  exit 2
fi
ce="$(cd "$1" && pwd)"
recipe="$ce/packages/multimedia/rtmpdump/package.mk"
if [[ ! -f "$recipe" ]]; then
  echo "Missing pinned rtmpdump recipe: $recipe" >&2
  exit 2
fi

version="f1b83c10d8beb43fcc70a6e88cf4325499f25857"
expected_sha="c68e05989a93c002e3ba8df3baef0021c17099aa2123a9c096a5cc8e029caf95"
expected_bytes=143043
old_url='PKG_URL="http://repo.or.cz/rtmpdump.git/snapshot/${PKG_VERSION}.tar.gz"'
new_url='PKG_URL="https://distfiles-origin.macports.org/rtmpdump/${PKG_VERSION}.tar.gz"'
source_url="https://distfiles-origin.macports.org/rtmpdump/${version}.tar.gz"
fallback_url="https://mirrors.mit.edu/macports/distfiles/rtmpdump/${version}.tar.gz"

# Fail closed if Pannal changes the requested revision or checksum. No
# checksum relaxation, no change to compiler flags, and no package upgrade.
grep -Fxq "PKG_VERSION=\"${version}\"" "$recipe" || {
  echo "Unexpected rtmpdump revision; refusing to alter its source URL" >&2
  exit 1
}
grep -Fxq "PKG_SHA256=\"${expected_sha}\"" "$recipe" || {
  echo "Unexpected rtmpdump SHA256; refusing to alter its source URL" >&2
  exit 1
}
if [[ "$(grep -c '^PKG_URL=' "$recipe" || true)" != 1 ]]; then
  echo "Unexpected number of rtmpdump source URLs in recipe" >&2
  exit 1
fi
if grep -Fxq "$old_url" "$recipe"; then
  sed -i "s@^PKG_URL=.*@${new_url}@" "$recipe"
elif ! grep -Fxq "$new_url" "$recipe"; then
  echo "Unknown rtmpdump source URL; refusing to overwrite Pannal changes" >&2
  exit 1
fi
grep -Fxq "$new_url" "$recipe"
echo "Pinned rtmpdump recipe now uses MacPorts; original SHA256 is unchanged."

if [[ "${2:---patch}" != "--fetch" ]]; then
  exit 0
fi

# CoreELEC's get_archive requires BOTH the source file AND its .url/.sha256
# stamps. Without the stamps it retries the original mirrors, even if a good
# tarball is already present.
source_dir="$ce/sources/rtmpdump"
mkdir -p "$source_dir"
archive="$source_dir/rtmpdump-${version}.tar.gz"

verified() {
  local file="$1"
  [[ -f "$file" ]] &&
    [[ "$(wc -c < "$file" | tr -d ' ')" == "$expected_bytes" ]] &&
    printf '%s  %s\n' "$expected_sha" "$file" | sha256sum --check --status &&
    tar -tzf "$file" >/dev/null
}
if ! verified "$archive"; then
  rm -f "$archive" "$archive.url" "$archive.sha256"
  temp="$(mktemp "${source_dir}/.rtmpdump.XXXXXX")"
  trap 'rm -f "$temp"' EXIT
  downloaded=0
  for url in "$source_url" "$fallback_url"; do
    echo "Fetching pinned rtmpdump archive: $url"
    if curl --fail --location --silent --show-error --connect-timeout 15 \
        --max-time 100 --retry 1 --retry-delay 2 --output "$temp" "$url" &&
        verified "$temp"; then
      mv -f "$temp" "$archive"
      downloaded=1
      break
    fi
    echo "Source mirror failed or archive checksum/format mismatch; trying another mirror" >&2
  done
  if (( !downloaded )); then
    echo "Unable to obtain verified pinned rtmpdump archive from MacPorts mirrors" >&2
    exit 1
  fi
fi

# These match CoreELEC scripts/get_archive's expected source stamps.
printf '%s\n' "$source_url" > "$archive.url"
printf '%s\n' "$expected_sha" > "$archive.sha256"
echo "Verified rtmpdump ${version}: ${expected_bytes} bytes, SHA256 ${expected_sha}"
