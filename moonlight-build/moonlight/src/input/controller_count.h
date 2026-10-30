/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Matches the controller slot limit in evdev and the GameStream mask. */
#define MOONLIGHT_GAMEPAD_SLOTS 16U

static inline int moonlight_gamepad_mask(int native, int sdl) {
  unsigned int count = native > 0 ? (unsigned int)native : 0U;
  if (count > MOONLIGHT_GAMEPAD_SLOTS)
    count = MOONLIGHT_GAMEPAD_SLOTS;
  if (sdl > 0) {
    const unsigned int remaining = MOONLIGHT_GAMEPAD_SLOTS - count;
    const unsigned int additional = (unsigned int)sdl;
    count += additional > remaining ? remaining : additional;
  }
  /* count <= 16; shifting an unsigned 32-bit value is defined. */
  return (int)((1U << count) - 1U);
}
