/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_REMOTE_INPUT_H
#define MOONLIGHT_REMOTE_INPUT_H

#include <stdbool.h>
#include <string.h>

/*
 * The Ugoos UR-02 is a composite Bluetooth HID and can advertise keyboard
 * capabilities even though it is used as a navigation remote. Match its
 * stable Linux device-name family explicitly so its Back key remains local.
 */
static inline bool moonlight_is_known_navigation_remote(const char *name) {
  if (!name)
    return false;

  /* CoreELEC exposes HDMI-CEC remote key events through this evdev device.
   * Its capability bitmap is intentionally sparse, so the generic
   * direction+enter heuristic does not identify it as a navigation remote. */
  if (strcmp(name, "cec_input") == 0)
    return true;

  if (!strstr(name, "Ugoos"))
    return false;

  return strstr(name, "UR-02") != NULL ||
         strstr(name, "UR02") != NULL ||
         strstr(name, "UR 02") != NULL;
}

/*
 * Classify a navigation-oriented HID without relying on vendor/product IDs.
 * Real gamepads and full keyboards keep their host-facing Back/Escape keys.
 */
static inline bool moonlight_is_navigation_remote(bool is_keyboard,
                                                  bool is_gamepad,
                                                  bool has_back_or_exit,
                                                  bool has_direction,
                                                  bool has_enter) {
  return !is_keyboard && !is_gamepad &&
         has_back_or_exit && has_direction && has_enter;
}

#endif
