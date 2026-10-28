/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_AUDIO_DIAGNOSTICS_H
#define MOONLIGHT_AUDIO_DIAGNOSTICS_H
#include <stdint.h>

/* Audio callback publishes scalars. The monitor never touches the live PCM. */
typedef struct {
  uint32_t active, callbacks, last_callback_ms, slow_20ms, slow_50ms;
  uint32_t callback_gaps_50ms;
  uint32_t writes, eagain, xruns, recover_attempts, recoveries;
  uint32_t short_writes, failed_packets, decode_errors;
  uint32_t sample_rate, delay_valid, delay_sample_ms, pcm_state, resync;
  int32_t delay_frames;
} AudioDiagnostics;

void alsa_diagnostics_snapshot(AudioDiagnostics *out);
#endif
