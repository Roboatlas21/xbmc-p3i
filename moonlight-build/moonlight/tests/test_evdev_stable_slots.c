/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>

/* Device entries are deliberately smaller than the actual evdev structure;
 * the table implementation is identical and cannot depend on its size. */
struct input_device { int identity; int alive; };
#include "../src/input/stable_slots.h"
static volatile int worker_done;
static void *reader(void *arg) {
  struct input_device *held = arg;
  while (!__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE)) {
    assert(held->alive == 1);
    assert(held->identity == 42);
    sched_yield();
  }
  return NULL;
}

int main(void) {
  struct input_device **devices = NULL;
  int count = 0;
  struct input_device *first = calloc(1, sizeof(*first));
  assert(first);
  first->identity = 42;
  first->alive = 1;
  assert(input_slots_append(&devices, &count, first));
  pthread_t worker;
  assert(pthread_create(&worker, NULL, reader, first) == 0);
  for (int i = 0; i < 128; i++) {
    struct input_device *other = calloc(1, sizeof(*other));
    assert(other);
    other->identity = i + 100;
    other->alive = 1;
    assert(input_slots_append(&devices, &count, other));
    assert(devices[0] == first);
  }
  struct input_device *detached = input_slots_remove(devices, &count, 17);
  assert(detached && detached->alive);
  free(detached);
  assert(devices[0] == first);
  assert(input_slots_remove(devices, &count, -1) == NULL);
  assert(input_slots_remove(devices, &count, count) == NULL);
  __atomic_store_n(&worker_done, 1, __ATOMIC_RELEASE);
  pthread_join(worker, NULL);
  first->alive = 0;
  while (count > 0) free(input_slots_remove(devices, &count, count - 1));
  free(devices);
  puts("PASS: stable hotplug table keeps live worker pointers valid");
  return 0;
}
