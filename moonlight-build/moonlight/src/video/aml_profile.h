/* Opt-in Amlogic submission measurements. GPL-3.0-or-later. */
#ifndef AML_PROFILE_H
#define AML_PROFILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define AML_PROFILE_BUCKETS 11

typedef struct {
  bool enabled;
  uint64_t frames;
  uint64_t bytes;
  uint64_t assembly_us;
  uint64_t submit_us;
  uint64_t submit_max_us;
  uint64_t write_us;
  uint64_t eagain;
  uint64_t write_calls;
  uint64_t backpressure_wait_us;
  uint64_t backpressure_polls;
  uint64_t backpressure_drops;
  uint64_t failed_frames;
  uint64_t submit_histogram[AML_PROFILE_BUCKETS];
  uint32_t dqbuf_cycles;
  uint32_t dqbuf_eagain;
} AmlProfile;

typedef struct {
  size_t bytes;
  uint64_t assembly_us;
  uint64_t submit_us;
  uint64_t write_us;
  unsigned int eagain;
  unsigned int write_calls;
  uint64_t backpressure_wait_us;
  unsigned int backpressure_polls;
  bool failed;
  bool dropped;
} AmlProfileSample;

uint64_t aml_profile_now_us(void);
void aml_profile_init(AmlProfile *profile, const char *enable);
void aml_profile_record(AmlProfile *profile, const AmlProfileSample *sample);
void aml_profile_display_poll(AmlProfile *profile, bool dequeued);
void aml_profile_report(const AmlProfile *profile, FILE *output);

#endif
