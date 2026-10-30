/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/video/aml_pacing_fault.h"
#include <assert.h>

int main(void) {
  unsigned int failures = 0;
  assert(!aml_pacing_fatal_poll(0, 0, 0, &failures));
  assert(!aml_pacing_fatal_poll(-1, 0, EINTR, &failures));
  assert(!aml_pacing_fatal_poll(1, POLLOUT, 0, &failures));
  assert(aml_pacing_fatal_poll(1, POLLNVAL, 0, &failures));
  assert(aml_pacing_fatal_poll(1, POLLERR | POLLOUT, 0, &failures));
  assert(aml_pacing_fatal_poll(-1, 0, EBADF, &failures));
  failures = 0;
  assert(!aml_pacing_fatal_poll(-1, 0, EIO, &failures));
  assert(!aml_pacing_fatal_poll(-1, 0, EIO, &failures));
  assert(aml_pacing_fatal_poll(-1, 0, EIO, &failures));
  failures = 0;
  assert(!aml_pacing_fatal_dequeue(EAGAIN, &failures));
  assert(!aml_pacing_fatal_dequeue(EINTR, &failures));
  assert(!aml_pacing_fatal_dequeue(EIO, &failures));
  assert(failures == 1);
  assert(!aml_pacing_fatal_dequeue(EAGAIN, &failures));
  assert(failures == 0);
  assert(!aml_pacing_fatal_dequeue(EIO, &failures));
  assert(!aml_pacing_fatal_dequeue(EAGAIN, &failures));
  assert(!aml_pacing_fatal_dequeue(EIO, &failures));
  assert(failures == 1);
  assert(!aml_pacing_fatal_dequeue(EAGAIN, &failures));
  assert(!aml_pacing_fatal_dequeue(EIO, &failures));
  assert(!aml_pacing_fatal_dequeue(EIO, &failures));
  assert(aml_pacing_fatal_dequeue(EIO, &failures));
  assert(aml_pacing_fatal_dequeue(ENODEV, &failures));
  return 0;
}
