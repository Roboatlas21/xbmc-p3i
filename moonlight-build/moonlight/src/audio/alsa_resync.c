/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "alsa_resync.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>

#define ALSA_RESYNC_MAX_WRITES 16
#define ALSA_RESYNC_MAX_WAITS 2
#define ALSA_RESYNC_MAX_RECOVERS 2
#define ALSA_RESYNC_WAIT_MS 60

static int reset_pcm(const AlsaResyncOps *ops, void *pcm,
                     AlsaResyncStats *stats) {
  if (ops->drop(pcm) < 0 || ops->prepare(pcm) < 0)
    return -1;
  stats->resets++;
  return 0;
}

long alsa_resync_write(const AlsaResyncOps *ops, void *pcm,
                       const AlsaResyncConfig *config, AlsaResyncStats *stats,
                       const void *audio, unsigned long frames) {
  if (!ops || !pcm || !config || !stats || (!audio && frames) ||
      !ops->avail || !ops->forward || !ops->write || !ops->wait ||
      !ops->recover || !ops->drop || !ops->prepare ||
      !config->period_frames || config->buffer_frames <= config->period_frames ||
      !config->bytes_per_frame ||
      frames > LONG_MAX || frames > SIZE_MAX / config->bytes_per_frame)
    return -EINVAL;
  if (!frames) return 0;

  const unsigned long buffer = config->buffer_frames;
  const unsigned long period = config->period_frames;
  const long avail = ops->avail(pcm);
  if (avail < 0 && avail != -EAGAIN) {
    if (avail == -EPIPE)
      stats->xruns++;
    stats->recover_attempts++;
    if (ops->recover(pcm, (int)avail) < 0) {
      stats->failed_packets++;
      stats->dropped_frames += frames;
      return -EIO;
    }
    stats->recoveries++;
  } else if (avail >= 0 && (unsigned long)avail > buffer) {
    /* Beyond buffer capacity means PCM playback has advanced ahead of the
     * application pointer. Advance to a fresh position before queuing audio.
     * Vortex uses one period for stereo, buffer-minus-period for surround. */
    const unsigned long excess = (unsigned long)avail - buffer;
    unsigned long headroom = config->channels > 2 ? buffer - period : period;
    if (headroom > buffer - period) headroom = buffer - period;
    int reset = 0;
    if (excess > buffer || excess > ULONG_MAX - headroom ||
        excess + headroom > LONG_MAX) {
      reset = 1; /* Stale too far to forward safely: discard the queue. */
    } else {
      const unsigned long count = excess + headroom;
      const long advanced = ops->forward(pcm, count);
      if (advanced == (long)count) {
        stats->events++;
        stats->forwarded_frames += count;
      } else {
        reset = 1; /* Includes unavailable/partial forwarding. */
      }
    }
    if (reset) {
      if (reset_pcm(ops, pcm, stats) < 0) {
        stats->failed_packets++;
        stats->dropped_frames += frames;
        return -EIO;
      }
      stats->events++;
    }
  }

  const unsigned char *position = audio;
  unsigned long remaining = frames;
  unsigned int writes = 0, waits = 0, recoveries = 0;
  while (remaining && writes++ < ALSA_RESYNC_MAX_WRITES) {
    const long rc = ops->write(pcm, position, remaining);
    stats->write_calls++;
    if (rc > 0) {
      if ((unsigned long)rc > remaining) break; /* Broken backend. */
      if ((unsigned long)rc < remaining)
        stats->short_writes++;
      remaining -= (unsigned long)rc;
      position += (size_t)rc * config->bytes_per_frame;
      continue;
    }
    long error = rc;
    if (rc == -EAGAIN)
      stats->eagain++;
    if (rc == -EPIPE)
      stats->xruns++;
    if (rc == 0 || rc == -EAGAIN) {
      if (waits++ >= ALSA_RESYNC_MAX_WAITS) break;
      stats->waits++;
      const int ready = ops->wait(pcm, ALSA_RESYNC_WAIT_MS);
      if (ready > 0) continue;
      if (ready == 0) {
        stats->wait_timeouts++;
        break;
      }
      error = ready;
      if (error == -EPIPE)
        stats->xruns++;
    }
    if (recoveries++ >= ALSA_RESYNC_MAX_RECOVERS)
      break;
    stats->recover_attempts++;
    if (ops->recover(pcm, (int)error) < 0)
      break;
    stats->recoveries++;
  }
  if (remaining) {
    stats->failed_packets++;
    stats->dropped_frames += remaining;
  }
  return (long)(frames - remaining);
}
