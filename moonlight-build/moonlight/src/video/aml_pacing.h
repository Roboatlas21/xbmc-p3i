/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_AML_PACING_H
#define MOONLIGHT_AML_PACING_H
#include <stdbool.h>

/* A dequeued V4L2 buffer remains owned by userspace until QBUF succeeds.
 * Advance the local ring only after the kernel accepted that buffer back. */
static inline bool aml_pacing_complete_qbuf(int qbuf_result,
                                            unsigned int capacity,
                                            unsigned int *head,
                                            unsigned int *count) {
  if (qbuf_result != 0 || !head || !count || capacity == 0 ||
      *count == 0 || *head >= capacity)
    return false;
  *head = (*head + 1U) % capacity;
  --*count;
  return true;
}
#endif
