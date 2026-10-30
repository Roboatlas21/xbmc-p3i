/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/osd/plane_ownership.h"
#include <assert.h>

/* Simulate an injected DRM atomic failure followed by a successful retry. */
static bool fake_commit(int fail) { return !fail; }

int main(void) {
  bool selected_plane = true, committed = false;
  if (fake_commit(1)) committed = true;
  assert(!osd_plane_disable_needed(selected_plane, committed));
  if (fake_commit(0)) committed = true;
  assert(osd_plane_disable_needed(selected_plane, committed));
  assert(!osd_plane_disable_needed(false, committed));
  return 0;
}
