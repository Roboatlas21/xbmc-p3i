/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_AML_WRITE_PROGRESS_H
#define MOONLIGHT_AML_WRITE_PROGRESS_H
#include <errno.h>

/* Zero-byte writes and EAGAIN both exhaust a bounded no-progress budget.
 * Successful partial writes reset that budget for the remaining bytes. */
typedef enum {
  AML_WRITE_FATAL = -1,
  AML_WRITE_RETRY = 0,
  AML_WRITE_PROGRESS = 1,
  AML_WRITE_TIMEOUT = 2
} AmlWriteProgress;

static inline AmlWriteProgress aml_write_progress(
    int bytes, int error, int remaining, unsigned int *no_progress,
    unsigned int max_no_progress) {
  if (!no_progress || max_no_progress == 0 || remaining <= 0)
    return AML_WRITE_FATAL;
  if (bytes > 0 && bytes <= remaining) {
    *no_progress = 0;
    return AML_WRITE_PROGRESS;
  }
  if (bytes == 0 || (bytes < 0 && error == EAGAIN)) {
    ++*no_progress;
    return *no_progress >= max_no_progress ? AML_WRITE_TIMEOUT : AML_WRITE_RETRY;
  }
  return AML_WRITE_FATAL;
}
#endif
