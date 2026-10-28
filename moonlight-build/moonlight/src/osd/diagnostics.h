/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_STREAM_DIAGNOSTICS_H
#define MOONLIGHT_STREAM_DIAGNOSTICS_H
#include "stats.h"
#include "../audio/diagnostics.h"

typedef struct {
  AmlFrameStats video;
  AudioDiagnostics audio;
  uint64_t start_ms, last_ms;
  bool initialized;
} StreamDiagnosticTracker;

/* One bounded line per second; input/native counters are not panel presents. */
void stream_diagnostics_format(StreamDiagnosticTracker *tracker, uint64_t now_ms,
                                bool native_available, const AmlFrameStats *video,
                                const OsdSnapshot *snapshot,
                                const AudioDiagnostics *audio,
                                char *out, size_t size);
#endif
