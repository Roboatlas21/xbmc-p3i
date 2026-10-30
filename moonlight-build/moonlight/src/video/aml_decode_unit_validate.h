/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* The caller includes Limelight.h for PDECODE_UNIT and PLENTRY.
 * Validate the aggregate byte count before copying ANY linked fragment. */
#define AML_MAX_DECODE_UNIT_BYTES (16U * 1024U * 1024U)
#define AML_MAX_DECODE_FRAGMENTS 4096U

static inline bool aml_decode_unit_valid(PDECODE_UNIT unit) {
  if (!unit || !unit->bufferList || unit->fullLength <= 0 ||
      (size_t)unit->fullLength > AML_MAX_DECODE_UNIT_BYTES)
    return false;
  size_t remaining = (size_t)unit->fullLength;
  unsigned int count = 0;
  for (PLENTRY fragment = unit->bufferList; fragment; fragment = fragment->next) {
    if (++count > AML_MAX_DECODE_FRAGMENTS || !fragment->data ||
        fragment->length <= 0 || (size_t)fragment->length > remaining)
      return false;
    remaining -= (size_t)fragment->length;
  }
  return remaining == 0;
}
