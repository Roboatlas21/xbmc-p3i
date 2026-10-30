/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Snapshot the axis' actual inactive value before prompting, rather than
 * assuming signed ABS ranges rest at zero. Some triggers idle at -32768. */
static inline bool moonlight_mapping_axis_pressed(int value, int rest,
                                                   int minimum, int maximum) {
  const int64_t span = (int64_t)maximum - minimum;
  if (span <= 0) return false;
  const int64_t delta = (int64_t)value - rest;
  return (delta < 0 ? -delta : delta) > span / 4;
}

static inline bool moonlight_mapping_axis_released(int value, int rest,
                                                    int minimum, int maximum) {
  const int64_t span = (int64_t)maximum - minimum;
  if (span <= 0) return false;
  const int64_t delta = (int64_t)value - rest;
  return (delta < 0 ? -delta : delta) <= span / 8;
}

/* 0 = full forward axis, '~' = full reversed axis, +/- = signed half-axis.
 * A trigger resting at either endpoint needs its full physical travel,
 * regardless of whether the kernel describes a signed or unsigned range. */
static inline char moonlight_mapping_trigger_mode(
    int minimum, int maximum, int rest, bool pressed_positive) {
  const int64_t span = (int64_t)maximum - minimum;
  if (span <= 0) return 0;
  if ((int64_t)rest <= (int64_t)minimum + span / 8 && pressed_positive)
    return 0;
  if ((int64_t)rest >= (int64_t)maximum - span / 8 && !pressed_positive)
    return '~';
  return pressed_positive ? '+' : '-';
}

static inline bool moonlight_mapping_hat_pressed(int direction) {
  return direction > 0 && direction <= 15;
}
