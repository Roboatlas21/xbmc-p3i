/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../src/input/handoff.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  assert(input_handoff_event_path("/dev/input/event0"));
  assert(input_handoff_event_path("/dev/input/event123"));
  assert(!input_handoff_event_path("/dev/input/event"));
  assert(!input_handoff_event_path("/dev/input/event3/driver"));
  assert(!input_handoff_event_path("/dev/input/mouse0"));
  assert(!input_handoff_event_path("/tmp/event3"));
  assert(!input_handoff_event_path(NULL));
  assert(input_handoff_retryable(EBUSY, 0));
  assert(input_handoff_retryable(EACCES, 2));
  assert(input_handoff_retryable(EAGAIN, 1));
  assert(!input_handoff_retryable(EBUSY, 3));
  assert(!input_handoff_retryable(ENODEV, 0));
  assert(!input_handoff_retryable(EINTR, 0));
  unsetenv("MOONLIGHT_INPUT_HANDOFF_PROBE");
  assert(!input_handoff_enabled("MOONLIGHT_INPUT_HANDOFF_PROBE"));
  setenv("MOONLIGHT_INPUT_HANDOFF_PROBE", "true", 1);
  assert(!input_handoff_enabled("MOONLIGHT_INPUT_HANDOFF_PROBE"));
  setenv("MOONLIGHT_INPUT_HANDOFF_PROBE", "1", 1);
  assert(input_handoff_enabled("MOONLIGHT_INPUT_HANDOFF_PROBE"));
  input_handoff_probe(NULL, 0); /* Works even without devices. */
  puts("Input handoff guard tests passed");
  return 0;
}
