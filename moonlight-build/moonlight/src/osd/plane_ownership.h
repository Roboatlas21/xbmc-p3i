/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>

/* Selecting a pre-attached Kodi plane does not grant Moonlight the right to
 * clear it. Ownership begins only after the first successful atomic commit. */
static inline bool osd_plane_disable_needed(bool acquired, bool committed) {
  return acquired && committed;
}
