/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/osd/layout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  OsdLayout r={0};
  osd_layout_wrap("Dec 60.0/s | Sink 60.0/s | Sub 60.0/s",
                  "Host 4ms | RTT 5ms", 24, 8, &r);
  assert(r.rows >= 3 && !r.truncated);
  for(unsigned i=0;i<r.rows;i++) {
    assert(strlen(r.text[i])<=24);
    assert(strlen(r.text[i])>0);
  }
  /* Each paragraph starts on its own row. */
  osd_layout_wrap("short", "next", 20, 8, &r);
  assert(r.rows==2 && !strcmp(r.text[0],"short") &&
         !strcmp(r.text[1],"next"));
  osd_layout_wrap("abcdefghijklmnopqrstuvwxyz",
                  "trailing text", 16, 8, &r);
  assert(r.rows>=3 && !r.truncated);
  assert(!strncmp(r.text[0],"abcdefghijklmnop",16));
  osd_layout_wrap("1234567890123456 1234567890123456 1234567890123456",
                  "more words",16,2,&r);
  assert(r.truncated && r.rows==2 &&
         strstr(r.text[r.rows-1], "..."));
  osd_layout_wrap(NULL,NULL,80,8,&r);
  assert(r.rows==0 && !r.truncated);
  puts("PASS: bounded OSD word wrapping, truncation and separate rows");
  return 0;
}
