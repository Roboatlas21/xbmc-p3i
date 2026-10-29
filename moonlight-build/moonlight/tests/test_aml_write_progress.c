/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/video/aml_write_progress.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  unsigned int n = 0;
  assert(aml_write_progress(0, 0, 100, &n, 3) == AML_WRITE_RETRY);
  assert(n == 1);
  assert(aml_write_progress(-1, EAGAIN, 100, &n, 3) == AML_WRITE_RETRY);
  assert(n == 2);
  assert(aml_write_progress(0, 0, 100, &n, 3) == AML_WRITE_TIMEOUT);
  assert(n == 3);
  assert(aml_write_progress(50, 0, 100, &n, 3) == AML_WRITE_PROGRESS);
  assert(n == 0);
  assert(aml_write_progress(0, 0, 50, &n, 2) == AML_WRITE_RETRY);
  assert(aml_write_progress(1, 0, 50, &n, 2) == AML_WRITE_PROGRESS);
  assert(n == 0);
  assert(aml_write_progress(60, 0, 50, &n, 3) == AML_WRITE_FATAL);
  assert(aml_write_progress(-1, EIO, 50, &n, 3) == AML_WRITE_FATAL);
  assert(aml_write_progress(0, 0, 0, &n, 3) == AML_WRITE_FATAL);
  puts("PASS: AML codec writes never spin on zero progress");
  return 0;
}
