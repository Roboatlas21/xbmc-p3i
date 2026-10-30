#include "../src/startup_cancel.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
static int interrupts = 0;
static int http_cancelled = 0;
void http_reset_pending_cancellation(void) { http_cancelled = 0; }
void http_cancel_pending_requests(void) { http_cancelled = 1; }
void LiInterruptConnection(void) {
  __atomic_fetch_add(&interrupts, 1, __ATOMIC_RELAXED);
}
int main(void) {
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  sigaddset(&blocked, SIGINT);
  sigaddset(&blocked, SIGHUP);
  sigaddset(&blocked, SIGQUIT);
  assert(!pthread_sigmask(SIG_BLOCK, &blocked, NULL));
  StartupCancellation state = {0};
  assert(startup_cancel_begin(&state));
  assert(kill(getpid(), SIGTERM) == 0);
  for (int i = 0; i < 30 && !__atomic_load_n(&interrupts, __ATOMIC_RELAXED); ++i)
    usleep(10000);
  assert(startup_cancel_end(&state));
  assert(__atomic_load_n(&interrupts, __ATOMIC_RELAXED) > 0);
  assert(http_cancelled == 1);
  puts("PASS: SIGTERM interrupts startup while all workers keep signals blocked");
  return 0;
}
