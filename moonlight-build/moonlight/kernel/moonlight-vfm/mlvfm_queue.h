/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MLVFM_QUEUE_H
#define MLVFM_QUEUE_H
#include "moonlight_vfm.h"

/* Callers serialize these operations. A peek pins the head until get: replacing
 * that frame between the sink's peek/get calls would invalidate its pointer. */
enum mlvfm_state { MLVFM_FREE, MLVFM_RESERVED, MLVFM_QUEUED,
                   MLVFM_PEEKED, MLVFM_HELD, MLVFM_DONE };
struct mlvfm_queue {
  enum mlvfm_state state[MLVFM_SLOTS];
  unsigned int ready[MLVFM_SLOTS];
  unsigned int count, limit;
};
static inline int mlvfm_reserve(struct mlvfm_queue *q, unsigned int i)
{
  if (i >= MLVFM_SLOTS || q->state[i] != MLVFM_FREE)
    return -1;
  q->state[i] = MLVFM_RESERVED;
  return 0;
}
/* Return dropped slot, or -1. Only a frame not yet peeked can be replaced. */
static inline int mlvfm_publish(struct mlvfm_queue *q, unsigned int i)
{
  unsigned int n;
  int dropped = -1;
  if (q->count == q->limit) {
    for (n = 0; n < q->count; ++n)
      if (q->state[q->ready[n]] == MLVFM_QUEUED)
        break;
    if (n == q->count) {
      q->state[i] = MLVFM_DONE;
      return i;
    }
    dropped = q->ready[n];
    q->state[dropped] = MLVFM_DONE;
    for (; n + 1 < q->count; ++n)
      q->ready[n] = q->ready[n + 1];
    --q->count;
  }
  q->state[i] = MLVFM_QUEUED;
  q->ready[q->count++] = i;
  return dropped;
}
static inline int mlvfm_peek(struct mlvfm_queue *q)
{
  unsigned int i;
  if (!q->count)
    return -1;
  i = q->ready[0];
  q->state[i] = MLVFM_PEEKED;
  return i;
}
static inline int mlvfm_get(struct mlvfm_queue *q)
{
  unsigned int n;
  int i = mlvfm_peek(q);
  if (i < 0)
    return i;
  for (n = 0; n + 1 < q->count; ++n)
    q->ready[n] = q->ready[n + 1];
  --q->count;
  q->state[i] = MLVFM_HELD;
  return i;
}
static inline int mlvfm_put(struct mlvfm_queue *q, unsigned int i)
{
  if (i >= MLVFM_SLOTS || q->state[i] != MLVFM_HELD)
    return -1;
  q->state[i] = MLVFM_DONE;
  return 0;
}
#endif
