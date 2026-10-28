/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/input/cec_keys.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  assert(moonlight_cec_media_action(113) == MOONLIGHT_CEC_MEDIA_MUTE);
  assert(moonlight_cec_media_action(114) == MOONLIGHT_CEC_MEDIA_VOLUME_DOWN);
  assert(moonlight_cec_media_action(115) == MOONLIGHT_CEC_MEDIA_VOLUME_UP);
  /* HDMI-CEC 0x41-0x43 are NOT Linux media keycodes. */
  assert(moonlight_cec_media_action(0x41) == MOONLIGHT_CEC_MEDIA_NONE);
  assert(moonlight_cec_media_action(0x42) == MOONLIGHT_CEC_MEDIA_NONE);
  assert(moonlight_cec_media_action(0x43) == MOONLIGHT_CEC_MEDIA_NONE);
  assert(moonlight_cec_media_action(0) == MOONLIGHT_CEC_MEDIA_NONE);
  puts("CEC local media key mapping: PASS");
  return 0;
}
