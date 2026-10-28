/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_INPUT_HANDOFF_H
#define MOONLIGHT_INPUT_HANDOFF_H
#include <stdbool.h>

/* A read-only /proc descriptor probe cannot tell whether EVIOCGRAB is active.
 * Log other opens, never assert exclusive ownership or modify a driver. */
bool input_handoff_enabled(const char *env_key);
bool input_handoff_event_path(const char *path);
bool input_handoff_retryable(int error, unsigned attempt);
void input_handoff_probe(const char *const *selected, int count);
#endif
