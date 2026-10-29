/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
pthread_t main_thread_id = 0;
#include "../src/loop.c"
static int first(int fd) { (void)fd; return LOOP_OK; }
static int second(int fd) { (void)fd; return LOOP_OK; }
static int third(int fd) { (void)fd; return LOOP_OK; }
int main(void) {
  loop_add_fd(10, first, POLLIN);
  loop_add_fd(20, second, POLLOUT);
  loop_add_fd(30, third, POLLERR);
  assert(numFds == 3);
  loop_remove_fd(99);
  assert(numFds == 3 && fds[0].fd == 10 && fds[1].fd == 20 && fds[2].fd == 30);
  assert(fdHandlers[1] == second);
  loop_remove_fd(20);
  assert(numFds == 2 && fds[1].fd == 30 && fdHandlers[1] == third);
  loop_remove_fd(20);
  assert(numFds == 2 && fds[0].fd == 10 && fds[1].fd == 30);
  loop_remove_fd(10);
  loop_remove_fd(30);
  loop_remove_fd(30);
  assert(numFds == 0);
  free(fds);
  free(fdHandlers);
  puts("PASS: event loop ignores unknown and duplicate descriptor removal");
  return 0;
}
