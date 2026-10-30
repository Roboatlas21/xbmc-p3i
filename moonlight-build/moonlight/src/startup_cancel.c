/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "startup_cancel.h"
#include "../libgamestream/http.h"
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Moonlight-common-c documents asynchronous cancellation of an in-progress
 * LiStartConnection. Do not call LiStopConnection concurrently from here. */
extern void LiInterruptConnection(void);

static void *startup_cancel_watch(void *arg) {
  StartupCancellation *state = arg;
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGTERM);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGHUP);
  sigaddset(&signals, SIGQUIT);
  while (!__atomic_load_n(&state->stopped, __ATOMIC_ACQUIRE)) {
    const struct timespec wait = {.tv_sec = 0, .tv_nsec = 100000000L};
    int signo = sigtimedwait(&signals, NULL, &wait);
    if (signo > 0) {
      http_cancel_pending_requests();
      __atomic_store_n(&state->cancelled, 1, __ATOMIC_RELEASE);
      /* Keep interrupting briefly: a signal can arrive immediately before
       * LiStartConnection enters its cancellable initialization. */
    } else if (errno != EAGAIN && errno != EINTR) {
      /* An unavailable signal waiter must not leave launch HTTP blocked. */
      http_cancel_pending_requests();
      fprintf(stderr, "Moonlight startup signal wait failed: %s\n", strerror(errno));
      __atomic_store_n(&state->cancelled, 1, __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&state->cancelled, __ATOMIC_ACQUIRE))
      LiInterruptConnection();
  }
  return NULL;
}

bool startup_cancel_begin(StartupCancellation *state) {
  if (!state) return false;
  state->running = false;
  http_reset_pending_cancellation();
  __atomic_store_n(&state->cancelled, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&state->stopped, 0, __ATOMIC_RELEASE);
  int rc = pthread_create(&state->thread, NULL, startup_cancel_watch, state);
  if (rc) {
    fprintf(stderr, "Moonlight startup signal thread failed: %s\n", strerror(rc));
    return false;
  }
  state->running = true;
  return true;
}

bool startup_cancel_requested(const StartupCancellation *state) {
  return state && __atomic_load_n(&state->cancelled, __ATOMIC_ACQUIRE) != 0;
}

bool startup_cancel_end(StartupCancellation *state) {
  if (!state || !state->running) return false;
  __atomic_store_n(&state->stopped, 1, __ATOMIC_RELEASE);
  pthread_join(state->thread, NULL);
  state->running = false;
  return __atomic_load_n(&state->cancelled, __ATOMIC_ACQUIRE) != 0;
}
