/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>

/* Sunshine's optional HttpsPort tag is untrusted XML. A missing/empty field
 * means the historical GameStream default; a malformed PRESENT value must
 * fail rather than silently truncate into unsigned short or fall back. */
#define MOONLIGHT_DEFAULT_HTTPS_PORT 47984U

static inline bool moonlight_parse_https_port(const char *text,
                                               unsigned short *port) {
  if (!port)
    return false;
  if (!text || !*text) {
    *port = (unsigned short)MOONLIGHT_DEFAULT_HTTPS_PORT;
    return true;
  }

  unsigned int value = 0;
  for (const unsigned char *c = (const unsigned char *)text; *c; ++c) {
    if (*c < '0' || *c > '9')
      return false;
    value = value * 10U + (unsigned int)(*c - '0');
    if (value > 65535U)
      return false; /* Detect overflow before multiplying again. */
  }
  if (!value)
    return false;
  *port = (unsigned short)value;
  return true;
}
