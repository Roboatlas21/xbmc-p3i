/* Exercise the same channel-map function used by the real Pulse renderer. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/audio/pulse_mapping.h"

int main(void) {
  const unsigned char opus[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  const unsigned char stereo[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  const unsigned char expected[8] = {0, 1, 4, 5, 2, 3, 6, 7};
  unsigned char actual[8];
  pulse_alsa_mapping(actual, opus, 2);
  assert(memcmp(actual, stereo, 8) == 0);
  pulse_alsa_mapping(actual, opus, 6);
  assert(memcmp(actual, expected, 8) == 0);
  pulse_alsa_mapping(actual, opus, 8);
  assert(memcmp(actual, expected, 8) == 0);
  puts("PASS: PulseAudio stereo, 5.1 and 7.1 independent of native ALSA reorder");
  return 0;
}
