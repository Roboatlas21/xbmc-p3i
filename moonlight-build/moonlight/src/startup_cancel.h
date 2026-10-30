/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <pthread.h>

typedef struct {
  pthread_t thread;
  bool running;
  int stopped;
  int cancelled;
} StartupCancellation;

/* For embedded clients: termination signals are already blocked in every
 * native worker. A temporary listener interrupts pending LiStartConnection()
 * rather than waiting until the main signalfd event loop starts. */
bool startup_cancel_begin(StartupCancellation *state);
bool startup_cancel_end(StartupCancellation *state);
bool startup_cancel_requested(const StartupCancellation *state);
