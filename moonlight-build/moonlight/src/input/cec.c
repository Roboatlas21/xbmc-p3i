/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include <Limelight.h>

#include <ceccloader.h>
#include "cec_keys.h"
#include "../loop.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define KEY_LEFT 0x25
#define KEY_UP 0x26
#define KEY_RIGHT 0x27
#define KEY_DOWN 0x28
#define KEY_ENTER 0x0D
#define KEY_TAB 0x09

static libcec_configuration g_config;
static char                 g_strPort[50] = { 0 };
static libcec_interface_t   g_iface;
static ICECCallbacks        g_callbacks;
static bool                 g_cec_open;
static bool                 g_cec_audio_system;

static void on_cec_keypress(void* userdata, const cec_keypress* key) {
  // CEC-originated navigation keys go to Sunshine. Local Linux EV_KEY
  // volume buttons are handled separately below through libCEC.
  int value = 0;
  switch (key->keycode) {
    case CEC_USER_CONTROL_CODE_UP:
      value = KEY_UP;
      break;
    case CEC_USER_CONTROL_CODE_DOWN:
      value = KEY_DOWN;
      break;
    case CEC_USER_CONTROL_CODE_LEFT:
      value = KEY_LEFT;
      break;
    case CEC_USER_CONTROL_CODE_RIGHT:
      value = KEY_RIGHT;
      break;
    case CEC_USER_CONTROL_CODE_ENTER:
    case CEC_USER_CONTROL_CODE_SELECT:
      value = KEY_ENTER;
      break;
    case CEC_USER_CONTROL_CODE_ROOT_MENU:
      value = KEY_TAB;
      break;
    case CEC_USER_CONTROL_CODE_AN_RETURN:
    case CEC_USER_CONTROL_CODE_EXIT:
      /* CEC is unambiguously a TV/AVR remote source. Back/Exit leaves the
       * local Moonlight session instead of sending Escape to Windows. */
      if (key->duration == 0)
        loop_request_stop();
      return;
    default:
      fprintf(stderr, "Unhandled CEC keycode: 0x%02x duration=%u\n",
              (unsigned int)key->keycode, (unsigned int)key->duration);
      fflush(stderr);
      value = 0;
      break;
  }

  if (value != 0) {
    unsigned short code = 0x80 << 8 | value;
    LiSendKeyboardEvent(code, (key->duration > 0)?KEY_ACTION_UP:KEY_ACTION_DOWN, 0);
  }
}

void cec_init(void) {
  libcecc_reset_configuration(&g_config);
  g_config.clientVersion = LIBCEC_VERSION_CURRENT;
  g_config.bActivateSource = 0;
  g_callbacks.keyPress = &on_cec_keypress;
  g_config.callbacks = &g_callbacks;
  snprintf(g_config.strDeviceName, sizeof(g_config.strDeviceName), "Moonlight");
  g_config.callbacks = &g_callbacks;
  g_config.deviceTypes.types[0] = CEC_DEVICE_TYPE_PLAYBACK_DEVICE;

  int init_result = libcecc_initialise(&g_config, &g_iface, NULL);
  if (init_result != 1) {
    fprintf(stderr, "Failed to initialize libcec interface (result=%d)\n",
            init_result);
    fflush(stderr);
    return;
  }

  g_iface.init_video_standalone(g_iface.connection);

  cec_adapter devices[10];
  int8_t iDevicesFound = g_iface.find_adapters(g_iface.connection, devices, sizeof(devices) / sizeof(devices[0]), NULL);

  if (iDevicesFound <= 0) {
    fprintf(stderr, "No CEC devices found\n");
    fflush(stderr);
    libcecc_destroy(&g_iface);
    return;
  }

  strcpy(g_strPort, devices[0].comm);
  if (!g_iface.open(g_iface.connection, g_strPort, 5000)) {
    fprintf(stderr, "Unable to open the device on port %s\n", g_strPort);
    fflush(stderr);
    libcecc_destroy(&g_iface);
    return;
  }

  // Do not force a new active source here: Kodi already owns HDMI routing,
  // and activating this client could wake the TV or steal its input.
  g_cec_audio_system = g_iface.poll_device != NULL &&
      g_iface.poll_device(g_iface.connection, CECDEVICE_AUDIOSYSTEM) > 0;
  g_cec_open = true;
  fprintf(stderr, "CEC adapter opened on %s\n", g_strPort);
  fflush(stderr);
}

void cec_shutdown(void) {
  if (!g_cec_open) return;
  libcecc_destroy(&g_iface);
  g_cec_open = false;
  g_cec_audio_system = false;
}

bool cec_handle_media_key(unsigned int linux_key, int value) {
  const enum MoonlightCecMediaAction action = moonlight_cec_media_action(linux_key);
  if (!g_cec_open || action == MOONLIGHT_CEC_MEDIA_NONE) return false;

  /* Consume key-up as well; don't send remote multimedia keys to the host
   * when we're controlling the local HDMI device. */
  if (value == 0) return true;

  if (action == MOONLIGHT_CEC_MEDIA_VOLUME_UP) {
    if (g_iface.volume_up) g_iface.volume_up(g_iface.connection, 1);
  } else if (action == MOONLIGHT_CEC_MEDIA_VOLUME_DOWN) {
    if (g_iface.volume_down) g_iface.volume_down(g_iface.connection, 1);
  } else if (action == MOONLIGHT_CEC_MEDIA_MUTE && value == 1) {
    /* Vortex uses an AVR/system-audio mute toggle when available, and
     * emits a CEC mute press/release to the TV otherwise. */
    if (g_cec_audio_system && g_iface.audio_toggle_mute) {
      g_iface.audio_toggle_mute(g_iface.connection);
    } else if (g_iface.send_keypress && g_iface.send_key_release) {
      g_iface.send_keypress(g_iface.connection, CECDEVICE_TV,
                             CEC_USER_CONTROL_CODE_MUTE, 1);
      g_iface.send_key_release(g_iface.connection, CECDEVICE_TV, 1);
    }
  }
  return true;
}
