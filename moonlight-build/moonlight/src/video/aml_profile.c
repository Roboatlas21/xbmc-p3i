/* Opt-in Amlogic submission measurements. GPL-3.0-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "aml_profile.h"
#include <inttypes.h>
#include <string.h>
#include <time.h>

/* Inclusive upper bounds, in microseconds. The last bucket is unbounded. */
static const uint64_t bucket_limit[AML_PROFILE_BUCKETS - 1] = {
  250, 500, 1000, 2000, 4000, 8000, 16000, 33000, 66000, 132000
};

uint64_t aml_profile_now_us(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void aml_profile_init(AmlProfile *profile, const char *enable) {
  memset(profile, 0, sizeof(*profile));
  profile->enabled = enable != NULL && strcmp(enable, "1") == 0;
}

void aml_profile_record(AmlProfile *profile, const AmlProfileSample *sample) {
  if (!profile->enabled) return;
  profile->frames++;
  profile->bytes += sample->bytes;
  profile->assembly_us += sample->assembly_us;
  profile->submit_us += sample->submit_us;
  profile->write_us += sample->write_us;
  if (sample->submit_us > profile->submit_max_us)
    profile->submit_max_us = sample->submit_us;
  profile->eagain += sample->eagain;
  profile->write_calls += sample->write_calls;
  profile->backpressure_wait_us += sample->backpressure_wait_us;
  profile->backpressure_polls += sample->backpressure_polls;
  profile->backpressure_drops += sample->dropped ? 1 : 0;
  profile->failed_frames += sample->failed ? 1 : 0;
  unsigned int i = 0;
  while (i < AML_PROFILE_BUCKETS - 1 && sample->submit_us > bucket_limit[i]) i++;
  profile->submit_histogram[i]++;
}

void aml_profile_display_poll(AmlProfile *profile, bool dequeued) {
  if (!profile->enabled) return;
  if (dequeued) __atomic_fetch_add(&profile->dqbuf_cycles, 1, __ATOMIC_RELAXED);
  else __atomic_fetch_add(&profile->dqbuf_eagain, 1, __ATOMIC_RELAXED);
}

static uint64_t percentile_upper(const AmlProfile *profile, unsigned percentile) {
  if (profile->frames == 0) return 0;
  uint64_t target = (profile->frames * percentile + 99) / 100;
  uint64_t count = 0;
  for (unsigned int i = 0; i < AML_PROFILE_BUCKETS; i++) {
    count += profile->submit_histogram[i];
    if (count >= target) return i < AML_PROFILE_BUCKETS - 1 ? bucket_limit[i] : 0;
  }
  return 0;
}

void aml_profile_report(const AmlProfile *profile, FILE *output) {
  if (!profile->enabled || profile->frames == 0) return;
  const uint64_t n = profile->frames;
  /* This measures submit/copy/write timing, NOT hardware display latency. */
  fprintf(output, "AML_PROFILE_V1 frames=%" PRIu64 " bytes=%" PRIu64
      " assembly_mean_us=%" PRIu64 " submit_mean_us=%" PRIu64
      " submit_max_us=%" PRIu64 " submit_p95_bucket_us=%" PRIu64
      " write_mean_us=%" PRIu64 " eagain=%" PRIu64
      " write_calls=%" PRIu64
      " backpressure_wait_us=%" PRIu64
      " backpressure_polls=%" PRIu64
      " backpressure_drops=%" PRIu64
      " failed=%" PRIu64
      " v4l2_dqbuf_cycles=%" PRIu32 " v4l2_poll_eagain=%" PRIu32 "\n",
      n, profile->bytes, profile->assembly_us / n, profile->submit_us / n,
      profile->submit_max_us, percentile_upper(profile, 95), profile->write_us / n,
      profile->eagain, profile->write_calls, profile->backpressure_wait_us,
      profile->backpressure_polls, profile->backpressure_drops,
      profile->failed_frames, profile->dqbuf_cycles, profile->dqbuf_eagain);
}
