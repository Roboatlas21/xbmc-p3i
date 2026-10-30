#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../src/input/controller_count.h"

int main(void) {
  assert(moonlight_gamepad_mask(0,0) == 0);
  assert(moonlight_gamepad_mask(1,0) == 1);
  assert(moonlight_gamepad_mask(2,1) == 7);
  assert(moonlight_gamepad_mask(0,16) == 65535);
  assert(moonlight_gamepad_mask(16,0) == 65535);
  assert(moonlight_gamepad_mask(17,0) == 65535);
  assert(moonlight_gamepad_mask(INT_MAX,INT_MAX) == 65535);
  assert(moonlight_gamepad_mask(-20,2) == 3);
  assert(moonlight_gamepad_mask(2,-5) == 3);
  assert(moonlight_gamepad_mask(8,8) == 65535);
  puts("PASS: controller count and bounded mask");
  return 0;
}
