/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/video/aml_pacing.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  unsigned int head = 3, count = 4;
  assert(!aml_pacing_complete_qbuf(-1, 16, &head, &count));
  assert(head == 3 && count == 4); /* Failed QBUF retains ownership/index. */

  assert(aml_pacing_complete_qbuf(0, 16, &head, &count));
  assert(head == 4 && count == 3);

  head = 15; count = 1;
  assert(aml_pacing_complete_qbuf(0, 16, &head, &count));
  assert(head == 0 && count == 0);

  head = 2; count = 0;
  assert(!aml_pacing_complete_qbuf(0, 16, &head, &count));
  assert(head == 2 && count == 0);
  puts("AML pacing keeps dequeued buffers until QBUF succeeds");
  return 0;
}
