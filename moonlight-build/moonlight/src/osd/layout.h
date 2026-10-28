/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_OSD_LAYOUT_H
#define MOONLIGHT_OSD_LAYOUT_H
#include <stdbool.h>
#include <stddef.h>
#define OSD_LAYOUT_MAX_ROWS 8
#define OSD_LAYOUT_MAX_COLS 160
typedef struct {
  char text[OSD_LAYOUT_MAX_ROWS][OSD_LAYOUT_MAX_COLS + 1];
  unsigned rows;
  bool truncated;
} OsdLayout;

/* Pure, bounded word wrapping; 2 separate paragraphs, maximum 8 rows. */
void osd_layout_wrap(const char *first, const char *second,
                     unsigned columns, unsigned max_rows, OsdLayout *out);
#endif
