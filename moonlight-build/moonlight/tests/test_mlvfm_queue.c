/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/moonlight-vfm/mlvfm_queue.h"

int main(void)
{
  struct mlvfm_queue q = {0};
  unsigned int n;
  /* ARM32/ARM64 ioctl layout must match exactly. */
  assert(sizeof(struct mlvfm_frame) == 64);
  assert(offsetof(struct mlvfm_frame, pts_us) == 24);
  assert(offsetof(struct mlvfm_frame, planes) == 32);
  assert(_IOC_SIZE(MLVFM_SUBMIT) == 64);
  q.limit = 1;
  assert(mlvfm_peek(&q) == -1);
  assert(mlvfm_reserve(&q, 0) == 0);
  assert(mlvfm_publish(&q, 0) == -1);
  assert(mlvfm_reserve(&q, 1) == 0);
  assert(mlvfm_publish(&q, 1) == 0); /* supersede unobserved frame */
  assert(q.state[0] == MLVFM_DONE);
  assert(mlvfm_peek(&q) == 1);
  assert(mlvfm_reserve(&q, 2) == 0);
  assert(mlvfm_publish(&q, 2) == 2); /* preserve peeked pointer */
  assert(mlvfm_get(&q) == 1);
  assert(q.state[1] == MLVFM_HELD);
  assert(mlvfm_reserve(&q, 1) == -1); /* no reuse before sink return */
  assert(mlvfm_put(&q, 1) == 0);
  assert(mlvfm_put(&q, 1) == -1); /* duplicate return rejected */
  assert(mlvfm_reserve(&q, 1) == -1); /* userspace still owns completion */
  q.state[1] = MLVFM_FREE; /* REAP consumed */
  assert(mlvfm_reserve(&q, 1) == 0);

  memset(&q, 0, sizeof(q));
  q.limit = 2;
  for (n = 0; n < 2; ++n) {
    assert(mlvfm_reserve(&q, n) == 0);
    assert(mlvfm_publish(&q, n) == -1);
  }
  assert(mlvfm_peek(&q) == 0);
  assert(mlvfm_reserve(&q, 2) == 0);
  assert(mlvfm_publish(&q, 2) == 1); /* replace tail, never pinned head */
  assert(mlvfm_get(&q) == 0);
  assert(mlvfm_get(&q) == 2);
  assert(mlvfm_get(&q) == -1);
  assert(mlvfm_put(&q, 0) == 0);
  assert(mlvfm_put(&q, 2) == 0);
  assert(mlvfm_reserve(&q, MLVFM_SLOTS) == -1);
  assert(mlvfm_put(&q, MLVFM_SLOTS) == -1);
  puts("VFM ownership, queue bounds and ioctl layout: OK");
  return 0;
}
