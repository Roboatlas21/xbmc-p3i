#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"${CC:-cc}" -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
  -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/libgamestream/xml.c" "$root/tests/test_native_xml_applist.c" \
  -lexpat -o "$tmp/test-native-xml"
ASAN_OPTIONS=detect_leaks=1 "$tmp/test-native-xml"

# Compile only xml.c with the allocator shim so the test's hook can call
# real realloc(). The fault must propagate as GS_OUT_OF_MEMORY.
"${CC:-cc}" -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
  -D_POSIX_C_SOURCE=200809L -Drealloc=xml_test_realloc \
  -fsanitize=address,undefined -c "$root/libgamestream/xml.c" -o "$tmp/xml-fault.o"
"${CC:-cc}" -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
  -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined \
  "$tmp/xml-fault.o" "$root/tests/test_native_xml_alloc.c" \
  -lexpat -o "$tmp/test-xml-alloc"
ASAN_OPTIONS=detect_leaks=1 "$tmp/test-xml-alloc"

# Check parser allocation failure without relying on an OOM-prone host.
"${CC:-cc}" -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
  -D_POSIX_C_SOURCE=200809L -DXML_ParserCreate=xml_test_parser_create \
  -fsanitize=address,undefined -c "$root/libgamestream/xml.c" -o "$tmp/xml-parser-fault.o"
"${CC:-cc}" -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter \
  -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined \
  "$tmp/xml-parser-fault.o" "$root/tests/test_native_xml_parser_failure.c" \
  -lexpat -o "$tmp/test-xml-parser-fault"
ASAN_OPTIONS=detect_leaks=1 "$tmp/test-xml-parser-fault"
