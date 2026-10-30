/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../src/connection.c"
/* This host-only callback test does not start the embedded event loop. */
static int termination_calls, error_termination_calls;
void loop_request_stop(void) { ++termination_calls; }
void loop_request_error_stop(void) { ++error_termination_calls; }
static int normal_calls, trigger_calls;
static void normal(unsigned short id, unsigned short low, unsigned short high) {
  (void)id; (void)low; (void)high;
  ++normal_calls;
}
static void triggers(unsigned short id, unsigned short left, unsigned short right) {
  (void)id; (void)left; (void)right;
  ++trigger_calls;
}
int main(void) {
  rumble_handler = normal;
  rumble_triggers_handler = NULL;
  connection_callbacks.rumbleTriggers(0, 1, 2);
  assert(normal_calls == 0 && trigger_calls == 0);
  rumble_triggers_handler = triggers;
  connection_callbacks.rumbleTriggers(0, 1, 2);
  assert(trigger_calls == 1);
  rumble_handler = NULL;
  connection_callbacks.rumbleTriggers(0, 1, 2);
  assert(trigger_calls == 2);
  rumble_triggers_handler = NULL;
  connection_callbacks.connectionTerminated(ML_ERROR_GRACEFUL_TERMINATION);
  assert(termination_calls == 1 && error_termination_calls == 0);
  connection_callbacks.connectionTerminated(ML_ERROR_NO_VIDEO_TRAFFIC);
  assert(termination_calls == 1 && error_termination_calls == 1);
  puts("PASS: rumble callback and normal/error connection teardown");
  return 0;
}
