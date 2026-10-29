/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise actual evdev lookup, rumble and remove functions in two threads.
 * The feedback ioctl pauses while the event loop tries to unplug. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/input.h>
#include <libevdev/libevdev.h>

static pthread_mutex_t test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cond = PTHREAD_COND_INITIALIZER;
static bool feedback_entered, release_feedback, remover_entered;

int __wrap_ioctl(int fd, unsigned long request, ...) {
  (void)fd;
  if (request == EVIOCSFF) {
    va_list args;
    va_start(args, request);
    struct ff_effect *effect = va_arg(args, struct ff_effect *);
    va_end(args);
    effect->id = 7;
    pthread_mutex_lock(&test_mutex);
    feedback_entered = true;
    pthread_cond_broadcast(&test_cond);
    while (!release_feedback)
      pthread_cond_wait(&test_cond, &test_mutex);
    pthread_mutex_unlock(&test_mutex);
  }
  return 0;
}

#include "../src/input/evdev.c"

void loop_remove_fd(int fd) { (void)fd; }
int LiSendMultiControllerEvent(short id, short mask, int buttons,
  unsigned char lt, unsigned char rt, short lx, short ly, short rx, short ry) {
  (void)id;(void)mask;(void)buttons;(void)lt;(void)rt;
  (void)lx;(void)ly;(void)rx;(void)ry;
  return 0;
}
static void *feedback(void *ignored) {
  (void)ignored;
  evdev_rumble(0, 32767, 10000);
  return NULL;
}
static void *disconnect(void *ignored) {
  (void)ignored;
  pthread_mutex_lock(&test_mutex);
  remover_entered = true;
  pthread_cond_broadcast(&test_cond);
  pthread_mutex_unlock(&test_mutex);
  evdev_remove(0);
  return NULL;
}

int main(void) {
  int pipefd[2];
  assert(pipe(pipefd) == 0);
  struct input_device *device = calloc(1, sizeof(*device));
  assert(device);
  device->dev = libevdev_new();
  assert(device->dev);
  libevdev_set_name(device->dev, "ASan mocked controller");
  device->controllerId = 0;
  device->haptic_effect_id = -1;
  device->fd = pipefd[1];
  assert(input_slots_append(&devices, &numDevices, device));
  assignedControllerIds = 1;
  pthread_t callback, unplug;
  assert(pthread_create(&callback, NULL, feedback, NULL) == 0);
  pthread_mutex_lock(&test_mutex);
  while (!feedback_entered)
    pthread_cond_wait(&test_cond, &test_mutex);
  pthread_mutex_unlock(&test_mutex);
  assert(pthread_create(&unplug, NULL, disconnect, NULL) == 0);
  pthread_mutex_lock(&test_mutex);
  while (!remover_entered)
    pthread_cond_wait(&test_cond, &test_mutex);
  pthread_mutex_unlock(&test_mutex);
  usleep(10000); /* Allow the remover to contend for the lifetime lock. */
  pthread_mutex_lock(&test_mutex);
  release_feedback = true;
  pthread_cond_broadcast(&test_cond);
  pthread_mutex_unlock(&test_mutex);
  pthread_join(callback, NULL);
  pthread_join(unplug, NULL);
  assert(numDevices == 0);
  /* Late feedback for an unplugged controller must be harmless. */
  evdev_rumble(0, 1000, 1000);
  free(devices);
  close(pipefd[0]);
  puts("PASS: async rumble cannot use device after controller removal");
  return 0;
}
