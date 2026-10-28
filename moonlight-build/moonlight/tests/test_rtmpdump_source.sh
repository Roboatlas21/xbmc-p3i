#!/usr/bin/env bash
# Exercise the exact-revision, exact-checksum, idempotent local source patch.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fix="$root/packaging/coreelec-ng/fix-rtmpdump-source.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
ce="$tmp/CoreELEC"
mkdir -p "$ce/packages/multimedia/rtmpdump"
recipe="$ce/packages/multimedia/rtmpdump/package.mk"
cat > "$recipe" <<'RECIPE'
PKG_NAME="rtmpdump"
PKG_VERSION="f1b83c10d8beb43fcc70a6e88cf4325499f25857"
PKG_SHA256="c68e05989a93c002e3ba8df3baef0021c17099aa2123a9c096a5cc8e029caf95"
PKG_URL="http://repo.or.cz/rtmpdump.git/snapshot/${PKG_VERSION}.tar.gz"
RECIPE

bash "$fix" "$ce" > "$tmp/result.txt"
grep -Fxq 'PKG_URL="https://distfiles-origin.macports.org/rtmpdump/${PKG_VERSION}.tar.gz"' "$recipe"
grep -Fxq 'PKG_SHA256="c68e05989a93c002e3ba8df3baef0021c17099aa2123a9c096a5cc8e029caf95"' "$recipe"
first="$(sha256sum "$recipe")"
bash "$fix" "$ce" >/dev/null
[[ "$(sha256sum "$recipe")" == "$first" ]]
[[ ! -d "$ce/sources/rtmpdump" ]]  # patch-only mode never uses the network

sed -i 's/c68e05989a93c002e3ba8df3baef0021c17099aa2123a9c096a5cc8e029caf95/bad-sha/' "$recipe"
if bash "$fix" "$ce" > "$tmp/reject-sha.log" 2>&1; then
  echo "Expected checksum mismatch refusal" >&2
  exit 1
fi
grep -Fq "Unexpected rtmpdump SHA256" "$tmp/reject-sha.log"

sed -i 's/PKG_SHA256="bad-sha"/PKG_SHA256="c68e05989a93c002e3ba8df3baef0021c17099aa2123a9c096a5cc8e029caf95"/' "$recipe"
sed -i 's/PKG_VERSION="f1b83c10d8beb43fcc70a6e88cf4325499f25857"/PKG_VERSION="other-version"/' "$recipe"
if bash "$fix" "$ce" > "$tmp/reject-version.log" 2>&1; then
  echo "Expected source revision mismatch refusal" >&2
  exit 1
fi
grep -Fq "Unexpected rtmpdump revision" "$tmp/reject-version.log"

sed -i 's/PKG_VERSION="other-version"/PKG_VERSION="f1b83c10d8beb43fcc70a6e88cf4325499f25857"/' "$recipe"
sed -i 's@https://distfiles-origin.macports.org/rtmpdump/@https://untrusted.example.invalid/rtmpdump/@' "$recipe"
if bash "$fix" "$ce" > "$tmp/reject-url.log" 2>&1; then
  echo "Expected unknown-URL refusal" >&2
  exit 1
fi
grep -Fq "Unknown rtmpdump source URL" "$tmp/reject-url.log"
echo "PASS: pinned rtmpdump MacPorts patch, exact SHA256, and safe refusals"
