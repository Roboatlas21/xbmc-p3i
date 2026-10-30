/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/input/mapping_capture.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  /* 0..255 resting at 0: full positive trigger. */
  assert(!moonlight_mapping_axis_pressed(0,0,0,255));
  assert(moonlight_mapping_axis_pressed(255,0,0,255));
  assert(!moonlight_mapping_axis_released(255,0,0,255));
  assert(moonlight_mapping_axis_released(0,0,0,255));
  assert(moonlight_mapping_trigger_mode(0,255,0,true) == 0);
  /* -32768..32767 resting at -32768: also a full positive trigger. */
  assert(moonlight_mapping_axis_pressed(32767,-32768,-32768,32767));
  assert(!moonlight_mapping_axis_released(0,-32768,-32768,32767));
  assert(moonlight_mapping_axis_released(-32768,-32768,-32768,32767));
  assert(moonlight_mapping_trigger_mode(-32768,32767,-32768,true) == 0);
  /* Reversed polarity: min is press, max is inactive. */
  assert(moonlight_mapping_axis_pressed(-32768,32767,-32768,32767));
  assert(moonlight_mapping_axis_released(32767,32767,-32768,32767));
  assert(moonlight_mapping_trigger_mode(-32768,32767,32767,false) == '~');
  assert(moonlight_mapping_trigger_mode(0,255,255,false) == '~');
  /* Centered sticks and actual mid-rest triggers still use half axes. */
  assert(moonlight_mapping_axis_pressed(32767,0,-32768,32767));
  assert(moonlight_mapping_axis_released(0,0,-32768,32767));
  assert(moonlight_mapping_trigger_mode(-32768,32767,0,true) == '+');
  assert(moonlight_mapping_trigger_mode(-32768,32767,0,false) == '-');
  assert(!moonlight_mapping_hat_pressed(0));
  assert(moonlight_mapping_hat_pressed(1));
  assert(moonlight_mapping_hat_pressed(15));
  assert(!moonlight_mapping_hat_pressed(16));
  puts("PASS: actual trigger rest, full-axis inversion, hats");
  return 0;
}
