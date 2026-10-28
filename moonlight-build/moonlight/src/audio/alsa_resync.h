/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_ALSA_RESYNC_H
#define MOONLIGHT_ALSA_RESYNC_H

#include <stddef.h>

/* The ALSA-facing wrappers use signed/unsigned long frame counts, matching
 * snd_pcm_sframes_t/snd_pcm_uframes_t on CoreELEC's Linux ARM userspace.
 * Keeping this interface ALSA-independent permits failure-injection tests. */
typedef struct {
  long (*avail)(void *pcm);
  long (*forward)(void *pcm, unsigned long frames);
  long (*write)(void *pcm, const void *audio, unsigned long frames);
  int (*wait)(void *pcm, int timeout_ms);
  int (*recover)(void *pcm, int error);
  int (*drop)(void *pcm);
  int (*prepare)(void *pcm);
} AlsaResyncOps;

typedef struct {
  unsigned long buffer_frames;
  unsigned long period_frames;
  unsigned int channels;
  size_t bytes_per_frame;
} AlsaResyncConfig;

typedef struct {
  unsigned long long events;
  unsigned long long forwarded_frames;
  unsigned long long resets;
  unsigned long long write_calls;
  unsigned long long eagain;
  unsigned long long xruns;
  unsigned long long recover_attempts;
  unsigned long long recoveries;
  unsigned long long short_writes;
  unsigned long long waits;
  unsigned long long wait_timeouts;
  unsigned long long dropped_frames;
  unsigned long long failed_packets;
} AlsaResyncStats;

/* Returns frames written, or a negative error. Never waits indefinitely. */
long alsa_resync_write(const AlsaResyncOps *ops, void *pcm,
                       const AlsaResyncConfig *config, AlsaResyncStats *stats,
                       const void *audio, unsigned long frames);

#endif
