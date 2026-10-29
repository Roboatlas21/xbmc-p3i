/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../src/audio/alsa_suspend_recovery.h"

static int resume_calls, prepare_calls, standard_calls, resume_result;
static int resume_one(void *ctx) {
  assert(ctx);
  ++resume_calls;
  return resume_result;
}
static int prepare_one(void *ctx) { assert(ctx); ++prepare_calls; return 0; }
static int recover_one(void *ctx, int error) {
  assert(ctx);
  assert(error == -EPIPE || error == -EINTR);
  ++standard_calls;
  return 0;
}
int main(void) {
  int pcm;
  AlsaRecoveryOps ops = {resume_one, prepare_one, recover_one};
  resume_result = -EAGAIN;
  assert(alsa_recover_bounded(&ops, &pcm, -ESTRPIPE) == 0);
  assert(resume_calls == 1 && prepare_calls == 1 && standard_calls == 0);
  resume_result = 0;
  assert(alsa_recover_bounded(&ops, &pcm, -ESTRPIPE) == 0);
  assert(resume_calls == 2 && prepare_calls == 1);
  resume_result = -ENOSYS;
  assert(alsa_recover_bounded(&ops, &pcm, -ESTRPIPE) == 0);
  assert(resume_calls == 3 && prepare_calls == 2);
  assert(alsa_recover_bounded(&ops, &pcm, -EPIPE) == 0);
  assert(alsa_recover_bounded(&ops, &pcm, -EINTR) == 0);
  assert(standard_calls == 2);
  assert(alsa_recover_bounded(NULL, &pcm, -ESTRPIPE) == -EINVAL);
  puts("PASS: ALSA suspend resumes once or prepares without waiting");
  return 0;
}
