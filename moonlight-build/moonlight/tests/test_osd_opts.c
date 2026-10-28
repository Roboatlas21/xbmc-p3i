/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/osd/osd.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 OsdOptions o;
 assert(osd_parse_options("-font-size 20 -y 40 -interval 1.5 -x 20 -log",&o));
 assert(o.x == 20 && o.y==40 && o.font_size==20 && o.log && o.interval_s>1.4);
 assert(osd_parse_options("",&o) && o.x==-1 && !o.log);
 assert(!osd_parse_options("-interval 0",&o));
 assert(!osd_parse_options("-x -20",&o));
 assert(!osd_parse_options("-unknown 20",&o));
 puts("OSD options tests passed");
}
