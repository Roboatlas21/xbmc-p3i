/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pulse_mapping.h"
#include <string.h>

void pulse_alsa_mapping(unsigned char output[8], const unsigned char input[8],
                        int channels) {
  memcpy(output, input, 8);
  if (channels >= 6) {
    output[2] = input[4];
    output[3] = input[5];
    output[4] = input[2];
    output[5] = input[3];
  }
}
