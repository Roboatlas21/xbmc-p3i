/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/audio/alsa_rate.h"
#include <assert.h>

static unsigned int mock_alsa_nearest(unsigned int requested, unsigned int device_rate) {
  /* The ALSA _near() API writes its negotiated rate to the caller. */
  (void)requested;
  return device_rate;
}
int main(void) {
  unsigned int negotiated=mock_alsa_nearest(48000,48000);
  assert(moonlight_alsa_rate_matches(48000,negotiated));
  negotiated=mock_alsa_nearest(48000,44100);
  assert(!moonlight_alsa_rate_matches(48000,negotiated));
  assert(!moonlight_alsa_rate_matches(0,48000));
  assert(!moonlight_alsa_rate_matches(48000,0));
  return 0;
}
