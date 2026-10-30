/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>

/* Reject malformed/overflowed numeric CLI and config values. strtol() alone
 * accepts trailing junk, and atoi() silently converts bad strings to zero. */
static inline bool moonlight_parse_bounded_int(const char *value, int minimum,
                                               int maximum, int *result) {
  if (!value || !*value || !result || minimum > maximum)
    return false;
  char *end = NULL;
  errno = 0;
  long parsed = strtol(value, &end, 10);
  if (errno == ERANGE || end == value || !end || *end != '\0' ||
      parsed < minimum || parsed > maximum)
    return false;
  *result = (int)parsed;
  return true;
}

/* These limits include conventional Moonlight resolutions through 8K and
 * 240 FPS without overflowing any 32-bit packet/bitrate calculations. */
static inline bool moonlight_stream_numbers_valid(int width, int height,
                                                  int fps, int packet_size,
                                                  int bitrate) {
  return width >= 64 && width <= 7680 &&
         height >= 64 && height <= 4320 &&
         fps >= 1 && fps <= 240 &&
         packet_size >= 256 && packet_size <= 1500 &&
         (bitrate == -1 || (bitrate >= 1 && bitrate <= 500000));
}

static inline uint64_t moonlight_stream_pixel_count(int width, int height) {
  /* Only call after validating positive dimensions. */
  return (uint64_t)(unsigned int)width * (uint64_t)(unsigned int)height;
}
