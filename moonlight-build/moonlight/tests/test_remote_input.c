/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/input/remote_input.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  /* Generic navigation remote: Back + direction + Enter, no keyboard/gamepad. */
  assert(moonlight_is_navigation_remote(false, false, true, true, true));

  /* A real gamepad must retain Back/View for the streamed application. */
  assert(!moonlight_is_navigation_remote(false, true, true, true, true));

  /* A full keyboard must retain Escape for the streamed application. */
  assert(!moonlight_is_navigation_remote(true, false, true, true, true));

  /* Do not classify arbitrary one-button HID devices as remotes. */
  assert(!moonlight_is_navigation_remote(false, false, true, false, false));
  assert(!moonlight_is_navigation_remote(false, false, false, true, true));

  /* CoreELEC CEC input has a sparse capability bitmap but is always local. */
  assert(moonlight_is_known_navigation_remote("cec_input"));

  /* UR-02 composite HID interfaces may advertise keyboard capabilities. */
  assert(moonlight_is_known_navigation_remote("UgoosUR02"));
  assert(moonlight_is_known_navigation_remote("Ugoos UR-02"));
  assert(moonlight_is_known_navigation_remote("Ugoos UR02 Consumer Control"));
  assert(!moonlight_is_known_navigation_remote("Generic Keyboard"));
  assert(!moonlight_is_known_navigation_remote("cec_input_extra"));
  assert(!moonlight_is_known_navigation_remote(NULL));

  puts("Remote input classification: PASS");
  return 0;
}
