/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../src/connection.c"
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
  puts("PASS: absent trigger-rumble callback is not dereferenced");
  return 0;
}
