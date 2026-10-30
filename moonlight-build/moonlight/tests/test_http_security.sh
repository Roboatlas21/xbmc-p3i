#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
command -v openssl >/dev/null
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$tmp/host.key" -out "$tmp/host.pem" -days 1 \
    -subj '/CN=Sunshine Test' >/dev/null 2>&1
cc -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
    -I./libgamestream tests/test_http_security.c -o "$tmp/test" \
    -lcurl -lssl -lcrypto
"$tmp/test" "$tmp" "$tmp/host.pem"
