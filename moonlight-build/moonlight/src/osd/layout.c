/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "layout.h"
#include <ctype.h>
#include <string.h>

static void truncate_marker(OsdLayout *out, unsigned columns) {
  out->truncated = true;
  if (!out->rows) return;
  char *last = out->text[out->rows - 1];
  size_t len = strlen(last);
  if (len + 3 > columns) len = columns - 3;
  memcpy(last + len, "...", 3);
  last[len + 3] = '\0';
}

static void wrap_paragraph(const char *input, unsigned columns,
                           unsigned max_rows, OsdLayout *out) {
  if (!input || !*input || out->truncated) return;
  if (out->rows == max_rows) {
    truncate_marker(out, columns);
    return;
  }
  char *line = out->text[out->rows++];
  unsigned length=0;
  const char *cursor=input;
  while (*cursor) {
    while (isspace((unsigned char)*cursor)) cursor++;
    if (!*cursor) break;
    const char *word=cursor;
    while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
    size_t left=(size_t)(cursor - word);
    while (left) {
      if (length && length + 1 >= columns) {
        if (out->rows == max_rows) {truncate_marker(out, columns);return;}
        line = out->text[out->rows++];
        length=0;
      }
      if (length && length + 1 + left <= columns)
        line[length++]=' ';
      else if (length && length + 1 + left > columns) {
        if (out->rows == max_rows) {truncate_marker(out, columns);return;}
        line = out->text[out->rows++];
        length=0;
      }
      const size_t chunk=left < columns-length ? left : columns-length;
      memcpy(line + length, word, chunk);
      length+=(unsigned)chunk;
      line[length]='\0';
      word+=chunk;
      left-=chunk;
      if (left) {
        if (out->rows == max_rows) {truncate_marker(out, columns);return;}
        line = out->text[out->rows++];
        length=0;
      }
    }
  }
}
void osd_layout_wrap(const char *first,const char *second,
                     unsigned columns,unsigned max_rows,OsdLayout *out) {
  if (!out) return;
  memset(out,0,sizeof(*out));
  if (columns<16) columns=16;
  if (columns>OSD_LAYOUT_MAX_COLS) columns=OSD_LAYOUT_MAX_COLS;
  if (!max_rows || max_rows>OSD_LAYOUT_MAX_ROWS) max_rows=OSD_LAYOUT_MAX_ROWS;
  wrap_paragraph(first,columns,max_rows,out);
  wrap_paragraph(second,columns,max_rows,out);
}
