#include "../src/video/aml_profile.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
  AmlProfile p;
  aml_profile_init(&p, NULL);
  AmlProfileSample a = { .bytes = 1024, .assembly_us = 2000,
    .submit_us = 1000, .write_us = 900, .eagain = 1, .write_calls = 2 };
  aml_profile_record(&p, &a);
  assert(p.frames == 0);
  aml_profile_init(&p, "1");
  for (int i = 0; i < 19; i++) aml_profile_record(&p, &a);
  a.submit_us = 70000;
  a.backpressure_wait_us = 30000;
  a.backpressure_polls = 6;
  a.failed = true;
  a.dropped = true;
  aml_profile_record(&p, &a);
  aml_profile_display_poll(&p, true);
  aml_profile_display_poll(&p, false);
  assert(p.frames == 20 && p.failed_frames == 1 && p.backpressure_drops == 1);
  assert(p.backpressure_wait_us == 30000 && p.backpressure_polls == 6);
  assert(p.submit_histogram[2] == 19);
  assert(p.submit_histogram[9] == 1);
  assert(p.dqbuf_cycles == 1 && p.dqbuf_eagain == 1);
  assert(aml_profile_now_us() > 0);
  aml_profile_report(&p, stdout);
  return 0;
}
