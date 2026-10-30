/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>

/* Opus produces PCM at its configured rate. ALSA's _set_rate_near() may
 * negotiate a different clock; without a resampler that stream is invalid. */
static inline bool moonlight_alsa_rate_matches(unsigned int opus_rate,
                                               unsigned int negotiated_rate) {
  return opus_rate != 0 && opus_rate == negotiated_rate;
}
