/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "mapping.h"
#include <stdbool.h>
#include <stddef.h>

/* Entries are indices in the evdev key/ABS tables, not kernel codes.
 * Require at least one control present on this specific controller.
 * Partial gamepad mappings remain valid. */
static inline bool moonlight_mapping_has_usable_bindings(
    const struct mapping *m, int buttons, int axes, unsigned int hats_mask) {
  if (!m || buttons < 0 || axes < 0) return false;
  const short keys[] = {
    m->btn_a, m->btn_b, m->btn_x, m->btn_y, m->btn_start, m->btn_back,
    m->btn_guide, m->btn_leftstick, m->btn_rightstick,
    m->btn_leftshoulder, m->btn_rightshoulder, m->btn_misc1,
    m->btn_paddle1, m->btn_paddle2, m->btn_paddle3, m->btn_paddle4,
    m->btn_touchpad, m->btn_lefttrigger, m->btn_righttrigger,
    m->btn_dpup, m->btn_dpdown, m->btn_dpleft, m->btn_dpright
  };
  const short axes_mapped[] = {
    m->abs_leftx, m->abs_lefty, m->abs_rightx, m->abs_righty,
    m->abs_lefttrigger, m->abs_righttrigger,
    m->abs_dpup, m->abs_dpdown, m->abs_dpleft, m->abs_dpright
  };
  const short hats[] = {m->hat_dpup, m->hat_dpdown, m->hat_dpleft, m->hat_dpright};
  const short dirs[] = {
    m->hat_dir_dpup, m->hat_dir_dpdown, m->hat_dir_dpleft, m->hat_dir_dpright
  };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
    if (keys[i] >= 0 && keys[i] < buttons) return true;
  for (size_t i = 0; i < sizeof(axes_mapped) / sizeof(axes_mapped[0]); ++i)
    if (axes_mapped[i] >= 0 && axes_mapped[i] < axes) return true;
  for (size_t i = 0; i < sizeof(hats) / sizeof(hats[0]); ++i)
    if (hats[i] >= 0 && hats[i] < 4 &&
        dirs[i] > 0 && dirs[i] <= 15 &&
        (hats_mask & (1U << hats[i]))) return true;
  return false;
}
