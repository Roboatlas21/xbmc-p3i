/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2019 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "loop.h"

#include "connection.h"

#include <sys/stat.h>
#include <sys/signalfd.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

static struct pollfd* fds = NULL;
static FdHandler* fdHandlers = NULL;
static int numFds = 0;

static int sigFd;
static bool loopReady;
static bool loopError;

static int loop_sig_handler(int fd) {
  struct signalfd_siginfo info;
  if (read(fd, &info, sizeof(info)) != sizeof(info))
    return LOOP_ERROR;
  switch (info.ssi_signo) {
    case SIGINT:
    case SIGTERM:
    case SIGQUIT:
    case SIGHUP:
      return LOOP_RETURN;
  }
  return LOOP_OK;
}

void loop_add_fd(int fd, FdHandler handler, int events) {
  int fdindex = numFds;
  numFds++;

  if (fds == NULL) {
    fds = malloc(sizeof(struct pollfd));
    fdHandlers = malloc(sizeof(FdHandler*));
  } else {
    fds = realloc(fds, sizeof(struct pollfd)*numFds);
    fdHandlers = realloc(fdHandlers, sizeof(FdHandler*)*numFds);
  }

  if (fds == NULL || fdHandlers == NULL) {
    fprintf(stderr, "Not enough memory\n");
    exit(EXIT_FAILURE);
  }

  fds[fdindex].fd = fd;
  fds[fdindex].events = events;
  /* Hotplug callbacks may append fds during dispatch before the next poll.
   * A new registration has no event until the next poll cycle. */
  fds[fdindex].revents = 0;
  fdHandlers[fdindex] = handler;
}

void loop_remove_fd(int fd) {
  /* Hotplug removal can be repeated. Unknown fds leave the list intact. */
  for (int i = 0; i < numFds; i++) {
    if (fds[i].fd != fd)
      continue;
    numFds--;
    if (i != numFds) {
      fds[i] = fds[numFds];
      fdHandlers[i] = fdHandlers[numFds];
    }
    return;
  }
}

static void termination_sigset(sigset_t *set) {
  sigemptyset(set);
  sigaddset(set, SIGHUP);
  sigaddset(set, SIGTERM);
  sigaddset(set, SIGINT);
  sigaddset(set, SIGQUIT);
}

void loop_block_termination_signals(void) {
  sigset_t set;
  termination_sigset(&set);
  /* A newly-created worker inherits this thread's signal mask. Blocking
   * only in loop_init() is too late for libCEC and evdev worker threads. */
  int result = pthread_sigmask(SIG_BLOCK, &set, NULL);
  if (result != 0) {
    errno = result;
    perror("Unable to block Moonlight termination signals");
    exit(EXIT_FAILURE);
  }
}

void loop_init() {
  __atomic_store_n(&loopError, false, __ATOMIC_RELEASE);
  main_thread_id = pthread_self();
  loop_block_termination_signals(); /* Also safe for callers without workers. */
  sigset_t set;
  termination_sigset(&set);
  sigFd = signalfd(-1, &set, SFD_CLOEXEC);
  if (sigFd < 0) {
    perror("Moonlight signalfd");
    exit(EXIT_FAILURE);
  }
  loop_add_fd(sigFd, loop_sig_handler, POLLIN | POLLERR | POLLHUP);
  loopReady = true;
}

void loop_request_stop(void) {
  if (loopReady)
    pthread_kill(main_thread_id, SIGTERM);
}

void loop_request_error_stop(void) {
  __atomic_store_n(&loopError, true, __ATOMIC_RELEASE);
  loop_request_stop();
}

bool loop_stream_failed(void) {
  return __atomic_load_n(&loopError, __ATOMIC_ACQUIRE);
}

static int loop_exit_result(void) {
  return __atomic_load_n(&loopError, __ATOMIC_ACQUIRE) ? LOOP_ERROR : LOOP_OK;
}

int loop_main() {
  for (;;) {
    if (__atomic_load_n(&loopError, __ATOMIC_ACQUIRE))
      return LOOP_ERROR;
    int result = poll(fds, numFds, -1);
    if (result < 0) {
      if (errno == EINTR) continue;
      perror("Moonlight input poll failed");
      return LOOP_ERROR;
    }
    if (result == 0) continue;

    /* Callbacks may add or remove descriptors, including their own. Do not
     * retain pointers into reallocatable arrays across the callback. */
    for (int i = 0; i < numFds;) {
      const int fd = fds[i].fd;
      const FdHandler cb = fdHandlers[i];
      const short events = fds[i].revents;
      fds[i].revents = 0;
      if (!events) { ++i; continue; }
      if (events & POLLNVAL) {
        fprintf(stderr, "Moonlight input poll: invalid descriptor %d\n", fd);
        return LOOP_ERROR;
      }
      int status = cb(fd);
      if (status == LOOP_RETURN)
        return loop_exit_result();
      if (status == LOOP_ERROR)
        return LOOP_ERROR;
      /* loop_remove_fd swaps the final slot into the removed position.
       * Revisit that index if it no longer contains this registration. */
      if (i < numFds && fds[i].fd == fd && fdHandlers[i] == cb)
        ++i;
    }
  }
}
