/*
 * Temporary Amlogic decoder low-latency kernel tuning.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MOONLIGHT_AML_LOW_LATENCY_H
#define MOONLIGHT_AML_LOW_LATENCY_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  void *ctx;
  int (*read_text)(void *ctx, const char *path, char *text, size_t size);
  int (*write_text)(void *ctx, const char *path, const char *text);
} AmlLowLatencyIo;

typedef struct {
  AmlLowLatencyIo io;
  bool changed;
  char original[8];
} AmlLowLatency;

void aml_low_latency_init(AmlLowLatency *state, const AmlLowLatencyIo *io);
bool aml_low_latency_enable(AmlLowLatency *state);
void aml_low_latency_restore(AmlLowLatency *state);

#endif
