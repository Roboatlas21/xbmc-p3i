/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_ALSA_SUSPEND_RECOVERY_H
#define MOONLIGHT_ALSA_SUSPEND_RECOVERY_H
#include <errno.h>

/* snd_pcm_recover(-ESTRPIPE) can poll snd_pcm_resume() forever. Use at most
 * one nonblocking resume attempt, then prepare, which also resets a stale
 * queue. Preserve normal ALSA handling for underruns and interrupts. */
typedef struct {
  int (*resume)(void *pcm);
  int (*prepare)(void *pcm);
  int (*recover)(void *pcm, int error);
} AlsaRecoveryOps;

static inline int alsa_recover_bounded(const AlsaRecoveryOps *ops,
                                      void *pcm, int error) {
  if (!ops || !pcm || !ops->resume || !ops->prepare || !ops->recover)
    return -EINVAL;
  if (error == -ESTRPIPE) {
    int result = ops->resume(pcm);
    return result == 0 ? 0 : ops->prepare(pcm);
  }
  return ops->recover(pcm, error);
}
#endif
