/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_OSD_H
#define MOONLIGHT_OSD_H
#include <stdbool.h>
#include <pthread.h>

typedef struct {
  int x;
  int y;
  int font_size;
  double interval_s;
  bool log;
} OsdOptions;

typedef struct {
  pthread_t thread;
  bool started;
  volatile int stopping;
  OsdOptions options;
} Osd;

bool osd_parse_options(const char *input, OsdOptions *out);
/* Call after LiStartConnection(), stop BEFORE LiStopConnection(). */
bool osd_start(Osd *osd, const char *options);
void osd_stop(Osd *osd);

#endif
