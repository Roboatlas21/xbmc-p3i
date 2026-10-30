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
#include "cec_port.h"
#include "../loop.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

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
static bool                 g_stream_ready;
static bool                 g_cec_audio_system;
/* libCEC callbacks execute asynchronously and must drain before stream teardown. */
static pthread_mutex_t      g_callback_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t       g_callback_cond = PTHREAD_COND_INITIALIZER;
static unsigned             g_active_callbacks;

static void on_cec_keypress(void* userdata, const cec_keypress* key) {
  pthread_mutex_lock(&g_callback_mutex);
  if (!g_cec_open) {
    pthread_mutex_unlock(&g_callback_mutex);
    return;
  }
  g_active_callbacks++;
  pthread_mutex_unlock(&g_callback_mutex);
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
      goto callback_done;
    default:
      fprintf(stderr, "Unhandled CEC keycode: 0x%02x duration=%u\n",
              (unsigned int)key->keycode, (unsigned int)key->duration);
      fflush(stderr);
      value = 0;
      break;
  }

  if (value != 0 && __atomic_load_n(&g_stream_ready, __ATOMIC_ACQUIRE)) {
    unsigned short code = 0x80 << 8 | value;
    LiSendKeyboardEvent(code, (key->duration > 0)?KEY_ACTION_UP:KEY_ACTION_DOWN, 0);
  }
callback_done:
  pthread_mutex_lock(&g_callback_mutex);
  if (--g_active_callbacks == 0)
    pthread_cond_signal(&g_callback_cond);
  pthread_mutex_unlock(&g_callback_mutex);
}

void cec_init(void) {
  __atomic_store_n(&g_stream_ready, false, __ATOMIC_RELEASE);
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

  if (!cec_copy_port_name(g_strPort, sizeof(g_strPort),
                          devices[0].comm, sizeof(devices[0].comm))) {
    fprintf(stderr, "CEC adapter path is empty, unterminated or too long\n");
    libcecc_destroy(&g_iface);
    return;
  }
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
  pthread_mutex_lock(&g_callback_mutex);
  g_cec_open = true;
  pthread_mutex_unlock(&g_callback_mutex);
  fprintf(stderr, "CEC adapter opened on %s\n", g_strPort);
  fflush(stderr);
}

void cec_stream_ready(void) {
  /* The common-c input queue is only safe after connection startup passed. */
  __atomic_store_n(&g_stream_ready, true, __ATOMIC_RELEASE);
}

void cec_shutdown(void) {
  __atomic_store_n(&g_stream_ready, false, __ATOMIC_RELEASE);
  pthread_mutex_lock(&g_callback_mutex);
  if (!g_cec_open) {
    pthread_mutex_unlock(&g_callback_mutex);
    return;
  }
  g_cec_open = false; /* New callbacks cannot submit input. */
  pthread_mutex_unlock(&g_callback_mutex);
  /* Stop the libCEC worker before unloading its shared library. Draining
   * our callback counter after dlclose is too late: a callback returning
   * into unloaded libCEC code would be a use-after-unload. */
  if (g_iface.close)
    g_iface.close(g_iface.connection);
  pthread_mutex_lock(&g_callback_mutex);
  while (g_active_callbacks != 0)
    pthread_cond_wait(&g_callback_cond, &g_callback_mutex);
  pthread_mutex_unlock(&g_callback_mutex);
  libcecc_destroy(&g_iface);
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
