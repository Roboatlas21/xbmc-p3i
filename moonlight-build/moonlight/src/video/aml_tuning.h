/*
 * Amlogic read-only VFM probe and automatic HEVC tuning.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MOONLIGHT_AML_TUNING_H
#define MOONLIGHT_AML_TUNING_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  void *ctx;
  int (*read_text)(void *ctx, const char *path, char *text, size_t size);
  int (*write_text)(void *ctx, const char *path, const char *text);
} AmlTuningIo;

typedef struct {
  AmlTuningIo io;
  bool skip_active;
  bool ref_active;
  char original_skip[64];
  char original_ref[128];
} AmlTuning;

void aml_tuning_init(AmlTuning *state, const AmlTuningIo *io);
/* Probe is read-only; guarded HEVC tuning is automatic for HEVC streams. */
void aml_tuning_configure(AmlTuning *state, bool hevc, const char *probe);
void aml_tuning_restore(AmlTuning *state);

#endif
