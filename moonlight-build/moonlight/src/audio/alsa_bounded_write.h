/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_ALSA_BOUNDED_WRITE_H
#define MOONLIGHT_ALSA_BOUNDED_WRITE_H
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
  long (*write)(void *, const void *, unsigned long);
  int (*wait)(void *, int);
  int (*recover)(void *, int);
  void (*record)(void *, long, unsigned long);
} AlsaBoundedOps;
typedef struct {
  unsigned int writes, waits, recovery_attempts, successful_recoveries;
  unsigned long remaining;
  int last_error;
} AlsaBoundedResult;
static inline AlsaBoundedResult alsa_bounded_write(
    const AlsaBoundedOps *ops, void *ctx, const void *pcm,
    unsigned long frames, size_t frame_bytes) {
  AlsaBoundedResult out = {0};
  out.remaining = frames;
  if (!ops || !ops->write || !ops->wait || !ops->recover || !frame_bytes ||
      (!pcm && frames) || (frames && frame_bytes > SIZE_MAX / frames)) {
    out.last_error = -EINVAL;
    return out;
  }
  const unsigned char *pos = pcm;
  while (out.remaining && out.writes < 16) {
    long rc = ops->write(ctx, pos, out.remaining);
    out.writes++;
    if (ops->record) ops->record(ctx, rc, out.remaining);
    if (rc > 0 && (unsigned long)rc <= out.remaining) {
      pos += (size_t)rc * frame_bytes;
      out.remaining -= (unsigned long)rc;
      continue;
    }
    if (rc > 0) { out.last_error = -EIO; break; }
    out.last_error = (int)rc;
    if (rc == 0 || rc == -EAGAIN) {
      if (out.waits >= 2) break;
      out.waits++;
      int ready = ops->wait(ctx, 5);
      if (ready > 0) continue;
      if (ready == 0) break;
      out.last_error = ready;
    }
    if (out.recovery_attempts >= 2) break;
    out.recovery_attempts++;
    if (ops->recover(ctx, out.last_error) < 0) break;
    out.successful_recoveries++;
  }
  return out;
}
#endif
