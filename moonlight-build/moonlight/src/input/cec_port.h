/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_CEC_PORT_H
#define MOONLIGHT_CEC_PORT_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Never truncate a device path into the name of another adapter. */
static inline bool cec_copy_port_name(char *dest, size_t dest_size,
                                      const char *source, size_t source_size) {
  if (!dest || !dest_size || !source || !source_size)
    return false;
  const char *end = memchr(source, '\0', source_size);
  if (!end) return false;
  size_t length = (size_t)(end - source);
  if (length == 0 || length >= dest_size) return false;
  memcpy(dest, source, length + 1);
  return true;
}
#endif
