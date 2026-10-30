/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Use the real event loop with an injected poll backend. No hardware needed. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
pthread_t main_thread_id = 0;
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout);
#define poll fake_poll
#include "../src/loop.c"
#undef poll

static int mode, calls, step, second_calls;
static int second(int fd) { assert(fd == 20); ++second_calls; return LOOP_RETURN; }
static int add_during_dispatch(int fd) {
  assert(fd == 10);
  ++calls;
  loop_add_fd(20, second, POLLIN);
  /* The newly added fd may not inherit stale poll events. */
  assert(fds[1].revents == 0);
  return LOOP_OK;
}
static int remove_during_dispatch(int fd) {
  assert(fd == 10);
  ++calls;
  loop_remove_fd(10);
  return LOOP_OK;
}
static int unexpected(int fd) { (void)fd; ++calls; return LOOP_RETURN; }
static int failed_callback(int fd) { (void)fd; return LOOP_ERROR; }
static int fake_poll(struct pollfd *entries, nfds_t count, int timeout) {
  assert(timeout == -1);
  if (mode == 1) {
    if (step++ == 0) { errno = EINTR; return -1; }
    assert(count == 2);
    entries[1].revents = POLLIN;
    return 1;
  }
  if (mode == 5) {
    if (step++ == 0) {
      assert(count == 1);
      entries[0].revents = POLLIN;
    } else {
      assert(count == 2);
      entries[1].revents = POLLIN;
    }
    return 1;
  }
  if (mode == 2) {
    assert(count == 2);
    entries[0].revents = POLLIN;
    entries[1].revents = POLLIN;
    return 2;
  }
  if (mode == 3) { errno = EIO; return -1; }
  if (mode == 4) {
    assert(count == 1);
    entries[0].revents = POLLNVAL;
    return 1;
  }
  abort();
}
static void reset(void) {
  free(fds); free(fdHandlers);
  fds = NULL; fdHandlers = NULL; numFds = 0;
  calls = second_calls = step = 0;
}
int main(void) {
  reset();
  mode = 5;
  loop_add_fd(10, add_during_dispatch, POLLIN);
  /* First dispatch adds a new fd. The second poll delivers it. */
  assert(loop_main() == LOOP_OK);
  assert(calls == 1 && second_calls == 1);
  reset();
  mode = 2;
  loop_add_fd(10, remove_during_dispatch, POLLIN);
  loop_add_fd(20, second, POLLIN);
  assert(loop_main() == LOOP_OK);
  assert(calls == 1 && second_calls == 1);
  reset();
  mode = 1;
  loop_add_fd(10, unexpected, POLLIN);
  loop_add_fd(20, second, POLLIN);
  /* EINTR causes a retry without invoking any stale callbacks. */
  assert(loop_main() == LOOP_OK);
  assert(calls == 0 && second_calls == 1);
  reset();
  mode = 3;
  loop_add_fd(10, unexpected, POLLIN);
  assert(loop_main() == LOOP_ERROR);
  assert(calls == 0);
  reset();
  mode = 4;
  loop_add_fd(10, unexpected, POLLIN);
  assert(loop_main() == LOOP_ERROR);
  assert(calls == 0);
  reset();
  mode = 2;
  loop_add_fd(10, failed_callback, POLLIN);
  loop_add_fd(20, second, POLLIN);
  assert(loop_main() == LOOP_ERROR);
  assert(second_calls == 0);
  reset();
  /* A remote or VFM failure is a fatal signal even if the callback merely
   * requests a normal event-loop return. */
  reset();
  mode = 2;
  loop_add_fd(10, unexpected, POLLIN);
  loop_add_fd(20, second, POLLIN);
  loop_request_error_stop();
  assert(loop_main() == LOOP_ERROR);
  reset();
  puts("PASS: hotplug add/remove, EINTR, failed poll and POLLNVAL");
  return 0;
}
