/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Luna passes exact UTF-8 device names encoded as comma-separated hex.
 * Hex encoding prevents a controller name containing a delimiter from
 * escaping the allowlist. Malformed tokens never authorize a device. */
static inline int moonlight_hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static inline bool moonlight_selected_input(const char *allowlist,
                                             const char *device_name) {
  if (!allowlist || !device_name || !*device_name) return false;
  const size_t name_size = strlen(device_name);
  if (name_size > 255) return false;
  const char *item = allowlist;
  while (*item) {
    const char *end = strchr(item, ',');
    if (!end) end = item + strlen(item);
    if ((size_t)(end - item) == name_size * 2) {
      bool equal = true;
      for (size_t i = 0; i < name_size; ++i) {
        const int high = moonlight_hex_digit(item[i * 2]);
        const int low = moonlight_hex_digit(item[i * 2 + 1]);
        if (high < 0 || low < 0 ||
            (unsigned char)((high << 4) | low) !=
                (unsigned char)device_name[i]) {
          equal = false;
          break;
        }
      }
      if (equal) return true;
    }
    if (!*end) break;
    item = end + 1;
  }
  return false;
}

/* v2 keeps the reported name together with an evdev unique ID (U), physical
 * port (P), or explicitly shared name-only fallback (N). Each text field is
 * hex-encoded. A selected USB/Bluetooth device cannot be impersonated just by
 * taking its former /dev/input/eventN number. */
static inline bool moonlight_hex_field_equals(const char *encoded, size_t hex_len,
                                               const char *text) {
  if (!text) return false;
  const size_t text_len = strlen(text);
  if (!text_len || text_len > 255 || hex_len != 2 * text_len) return false;
  for (size_t i = 0; i < text_len; ++i) {
    const int high = moonlight_hex_digit(encoded[2*i]);
    const int low = moonlight_hex_digit(encoded[2*i+1]);
    if (high < 0 || low < 0 || (unsigned char)((high << 4) | low) !=
                                (unsigned char)text[i])
      return false;
  }
  return true;
}

static inline bool moonlight_selected_identity(const char *selectors,
                                                const char *legacy_names,
                                                const char *name,
                                                const char *uniq,
                                                const char *phys) {
  /* A v2 selector list is authoritative, even if a legacy list is present. */
  if (selectors) {
    if (!name || !*name || strlen(selectors) > 16384) return false;
    const char *item = selectors;
    while (*item) {
      const char *end = strchr(item, ',');
      if (!end) end = item + strlen(item);
      const char *first = memchr(item, ':', (size_t)(end - item));
      const char *second = first ?
          memchr(first + 1, ':', (size_t)(end - first - 1)) : NULL;
      const char *third = second ?
          memchr(second + 1, ':', (size_t)(end - second - 1)) : NULL;
      if (first && second && second - first == 2 &&
          moonlight_hex_field_equals(item, (size_t)(first - item), name)) {
        const char type = first[1];
        /* New U selectors optionally pin both serial and physical endpoint.
         * A generic serial shared by a later-attached controller cannot
         * impersonate the already-selected device on another port. Legacy
         * three-field U/P/N selectors remain compatible. */
        if (type == 'U') {
          const char *uniq_end = third ? third : end;
          if (moonlight_hex_field_equals(
                  second + 1, (size_t)(uniq_end - second - 1), uniq) &&
              (!third || moonlight_hex_field_equals(
                  third + 1, (size_t)(end - third - 1), phys)))
            return true;
        }
        if (type == 'P' && !third && moonlight_hex_field_equals(
                second + 1, (size_t)(end - second - 1), phys))
          return true;
        if (type == 'N' && !third && second + 1 == end)
          return true;
      }
      if (!*end) break;
      item = end + 1;
    }
    return false;
  }
  return legacy_names ? moonlight_selected_input(legacy_names, name) : true;
}
