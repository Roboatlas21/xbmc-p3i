/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <errno.h>
#include <poll.h>

/* A dead decoder fd can make poll/DQBUF fail immediately forever.
 * EINTR and ordinary EAGAIN are transient; other faults are bounded. */
static inline bool aml_pacing_fatal_poll(int result, short events, int error,
                                         unsigned int *failures) {
  if (result > 0) {
    *failures = 0;
    return (events & (POLLERR | POLLHUP | POLLNVAL)) != 0;
  }
  if (result == 0) {
    *failures = 0;
    return false;
  }
  if (error == EINTR) return false;
  if (error == EBADF || error == ENODEV) return true;
  return ++*failures >= 3;
}
static inline bool aml_pacing_fatal_dequeue(int error, unsigned int *failures) {
  if (error == EAGAIN) {
    *failures = 0; /* Normal empty queue breaks any earlier error streak. */
    return false;
  }
  if (error == EINTR) return false;
  if (error == EBADF || error == ENODEV) return true;
  return ++*failures >= 3;
}
