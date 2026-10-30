/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/input/mapping_usable.h"
#include <assert.h>
#include <string.h>

int main(void) {
  struct mapping m;
  memset(&m, 0xff, sizeof(m));
  assert(!moonlight_mapping_has_usable_bindings(NULL, 4, 6, 1));
  assert(!moonlight_mapping_has_usable_bindings(&m, 4, 6, 1));
  m.btn_a = 2;
  assert(moonlight_mapping_has_usable_bindings(&m, 4, 6, 1));
  assert(!moonlight_mapping_has_usable_bindings(&m, 2, 6, 1));
  m.btn_a = -1;
  m.abs_lefttrigger = 4;
  assert(moonlight_mapping_has_usable_bindings(&m, 4, 6, 1));
  m.abs_lefttrigger = 9;
  assert(!moonlight_mapping_has_usable_bindings(&m, 4, 6, 1));
  m.abs_lefttrigger = -1;
  m.hat_dpup = 1; m.hat_dir_dpup = 1;
  assert(moonlight_mapping_has_usable_bindings(&m, 0, 0, 2));
  assert(!moonlight_mapping_has_usable_bindings(&m, 0, 0, 1));
  m.hat_dir_dpup = 0;
  assert(!moonlight_mapping_has_usable_bindings(&m, 0, 0, 2));
  return 0;
}
