/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>

/* REL events may arrive several times before SYN_REPORT. Saturate rather
 * than invoking signed overflow on malformed or very large kernel deltas. */
static inline int32_t moonlight_saturating_delta(int64_t value) {
  if (value > INT32_MAX) return INT32_MAX;
  if (value < INT32_MIN) return INT32_MIN;
  return (int32_t)value;
}
/* Rotated mouse movement must not negate INT32_MIN (undefined behavior). */
static inline int32_t moonlight_negate_rel(int32_t value) {
  return value == INT32_MIN ? INT32_MAX : -value;
}
static inline int32_t moonlight_accumulate_rel(int32_t prior, int32_t delta) {
  return moonlight_saturating_delta((int64_t)prior + delta);
}
static inline int32_t moonlight_touch_delta(int32_t now, int32_t before) {
  return moonlight_saturating_delta((int64_t)now - before);
}
static inline bool moonlight_touch_is_tap(int32_t x, int32_t y,
                                            int32_t down_x, int32_t down_y,
                                            int radius) {
  if (radius <= 0) return false;
  const int64_t dx = (int64_t)x - down_x;
  const int64_t dy = (int64_t)y - down_y;
  /* Bound before squaring: extreme 32-bit differences do not fit in int64
   * once squared, and should never be classified as clicks anyway. */
  if (dx <= -(int64_t)radius || dx >= (int64_t)radius ||
      dy <= -(int64_t)radius || dy >= (int64_t)radius)
    return false;
  return dx*dx + dy*dy < (int64_t)radius*radius;
}
