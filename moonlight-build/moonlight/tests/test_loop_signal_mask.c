/* Ensure libCEC/evdev workers inherit the blocked process signal mask. */
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
pthread_t main_thread_id;
#include "../src/loop.c"
static void *read_worker_mask(void *unused) {
  (void)unused;
  sigset_t set;
  assert(pthread_sigmask(SIG_SETMASK, NULL, &set) == 0);
  for (int sig = 0; sig < 4; sig++) {
    const int termination[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT };
    assert(sigismember(&set, termination[sig]) == 1);
  }
  return NULL;
}
int main(void) {
  loop_block_termination_signals();
  pthread_t worker;
  assert(pthread_create(&worker, NULL, read_worker_mask, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  loop_init();
  assert(sigFd >= 0);
  close(sigFd);
  free(fds);
  free(fdHandlers);
  puts("PASS: termination signals blocked before worker creation");
  return 0;
}
