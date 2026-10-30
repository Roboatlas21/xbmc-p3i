/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/input/motion_math.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  int32_t pending = 0;
  pending = moonlight_accumulate_rel(pending, 3);
  pending = moonlight_accumulate_rel(pending, 4);
  assert(pending == 7);
  pending = moonlight_accumulate_rel(pending, -2);
  assert(pending == 5);
  assert(moonlight_accumulate_rel(INT32_MAX - 1, 50) == INT32_MAX);
  assert(moonlight_accumulate_rel(INT32_MIN + 1, -50) == INT32_MIN);
  assert(moonlight_negate_rel(0) == 0);
  assert(moonlight_negate_rel(-6) == 6);
  assert(moonlight_negate_rel(INT32_MIN) == INT32_MAX);
  assert(moonlight_negate_rel(INT32_MAX) == -INT32_MAX);
  assert(moonlight_touch_delta(INT32_MAX, INT32_MIN) == INT32_MAX);
  assert(moonlight_touch_delta(INT32_MIN, INT32_MAX) == INT32_MIN);
  assert(moonlight_touch_is_tap(4, 5, 0, 0, 10));
  assert(!moonlight_touch_is_tap(9, 9, 0, 0, 10));
  assert(!moonlight_touch_is_tap(INT32_MAX, 0, INT32_MIN, 0, 10));
  assert(!moonlight_touch_is_tap(0, INT32_MIN, 0, INT32_MAX, 10));
  assert(!moonlight_touch_is_tap(0, 0, 0, 0, 0));
  puts("PASS: relative delta accumulation and safe touch arithmetic");
  return 0;
}
