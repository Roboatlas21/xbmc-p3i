/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_STABLE_INPUT_SLOTS_H
#define MOONLIGHT_STABLE_INPUT_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>

struct input_device;

/* The table can move; its individually allocated device entries never do.
 * The event-loop thread owns additions/removals and joins any device worker
 * before freeing the removed entry. */
static inline bool input_slots_append(struct input_device ***table, int *count,
                                      struct input_device *device) {
  if (!table || !count || !device || *count < 0 || *count == INT_MAX)
    return false;
  size_t next_count = (size_t)*count + 1;
  if (next_count > SIZE_MAX / sizeof(**table))
    return false;
  struct input_device **next = realloc(*table, next_count * sizeof(**table));
  if (!next)
    return false;
  *table = next;
  next[*count] = device;
  ++*count;
  return true;
}

static inline struct input_device *input_slots_remove(
    struct input_device **table, int *count, int index) {
  if (!table || !count || index < 0 || index >= *count)
    return NULL;
  struct input_device *removed = table[index];
  --*count;
  table[index] = table[*count];
  table[*count] = NULL;
  return removed;
}
#endif
