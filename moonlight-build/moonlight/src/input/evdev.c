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

#include "evdev.h"
#include "handoff.h"
#include "stable_slots.h"
#include "remote_input.h"
#include "controller_count.h"
#include "mapping_capture.h"
#include "mapping_usable.h"
#include "selected_names.h"
#include "motion_math.h"

#include "keyboard.h"
#ifdef HAVE_LIBCEC
#include "cec.h"
#endif

#include "../loop.h"

#include "libevdev/libevdev.h"
#include <Limelight.h>

#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <fcntl.h>
#include <poll.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <pthread.h>
#ifdef __linux__
#include <endian.h>
#else
#include <sys/endian.h>
#endif
#include <math.h>

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define int16_to_le(val) val
#else
#define int16_to_le(val) ((((val) >> 8) & 0x00FF) | (((val) << 8) & 0xFF00))
#endif

struct input_abs_parms {
  int min, max;
  int flat;
  int avg;
  int range, diff;
};

struct input_device {
  struct libevdev *dev;
  bool is_keyboard;
  bool is_mouse;
  bool is_touchscreen;
  bool is_remote;
  bool is_gamepad;
  int rotate;
  struct mapping* map;
  int key_map[KEY_CNT];
  int abs_map[ABS_CNT];
  /* Linux defines ABS_HAT0..ABS_HAT3; the fourth hat is index 3. */
  int hats_state[4][2];
  int mapping_rest[ABS_CNT]; /* Snapshot before each mapping prompt. */
  int fd;
  char modifiers;
  #ifdef __linux__
  __s32 mouseDeltaX, mouseDeltaY, mouseVScroll, mouseHScroll;
  __s32 touchDownX, touchDownY, touchX, touchY;
  #else
  int32_t mouseDeltaX, mouseDeltaY, mouseVScroll, mouseHScroll;
  int32_t touchDownX, touchDownY, touchX, touchY;
  #endif
  struct timeval touchDownTime;
  struct timeval btnDownTime;
  short controllerId;
  int haptic_effect_id;
  int buttonFlags;
  unsigned char leftTrigger, rightTrigger;
  short leftStickX, leftStickY;
  short rightStickX, rightStickY;
  bool gamepadModified;
  bool mouseEmulation;
  pthread_t meThread;
  struct input_abs_parms xParms, yParms, rxParms, ryParms, zParms, rzParms;
  struct input_abs_parms leftParms, rightParms, upParms, downParms;
};

#define HAT_UP 1
#define HAT_RIGHT 2
#define HAT_DOWN 4
#define HAT_LEFT 8
static const int hat_constants[3][3] = {{HAT_UP | HAT_LEFT, HAT_UP, HAT_UP | HAT_RIGHT}, {HAT_LEFT, 0, HAT_RIGHT}, {HAT_LEFT | HAT_DOWN, HAT_DOWN, HAT_DOWN | HAT_RIGHT}};

#define set_hat(flags, flag, hat, hat_flag) flags = (hat & hat_flag) == hat_flag ? flags | flag : flags & ~flag

#define TOUCH_UP -1
#define TOUCH_CLICK_RADIUS 10
#define TOUCH_CLICK_DELAY 100000 // microseconds
#define TOUCH_RCLICK_TIME 750 // milliseconds

// How long the Start button must be pressed to toggle mouse emulation
#define MOUSE_EMULATION_LONG_PRESS_TIME 750
// How long between polling the gamepad to send virtual mouse input
#define MOUSE_EMULATION_POLLING_INTERVAL 50000
// Determines how fast the mouse will move each interval
#define MOUSE_EMULATION_MOTION_MULTIPLIER 3
// Determines the maximum motion amount before allowing movement
#define MOUSE_EMULATION_DEADZONE 2

// Limited by number of bits in activeGamepadMask
#define MAX_GAMEPADS 16

static struct input_device **devices = NULL;
/* The common-c rumble callback runs on another thread. Protect lookup,
 * publication, force-feedback I/O and removal under one lifetime lock. */
static pthread_mutex_t devices_mutex = PTHREAD_MUTEX_INITIALIZER;
static int numDevices = 0;
static int assignedControllerIds = 0;

static short* currentKey;
static short* currentHat;
static short* currentHatDir;
static short* currentAbs;
static bool* currentReverse;
static char* currentHalfAxis;

static bool grabbingDevices;
static bool mouseEmulationEnabled;

static bool waitingToExitOnModifiersUp = false;

int evdev_gamepads = 0;

#define ACTION_MODIFIERS (MODIFIER_SHIFT|MODIFIER_ALT|MODIFIER_CTRL)
#define QUIT_KEY KEY_Q
#define QUIT_BUTTONS (PLAY_FLAG|BACK_FLAG|LB_FLAG|RB_FLAG)

static bool (*handler) (struct input_event*, struct input_device*);

static int evdev_get_map(int* map, int length, int value) {
  for (int i = 0; i < length; i++) {
    if (value == map[i])
      return i;
  }
  return -1;
}

static bool evdev_init_parms(struct input_device *dev, struct input_abs_parms *parms, int code) {
  *parms = (struct input_abs_parms){0};
  if (code < 0) return true; /* Optional unmapped axes are neutral. */
  int abs_code = evdev_get_map(dev->abs_map, ABS_MAX, code);
  if (abs_code < 0) return false;

  const int min = libevdev_get_abs_minimum(dev->dev, abs_code);
  const int max = libevdev_get_abs_maximum(dev->dev, abs_code);
  const int flat = libevdev_get_abs_flat(dev->dev, abs_code);
  const int64_t span = (int64_t)max - (int64_t)min;
  /* Both signed stick and unsigned trigger scaling require a positive
   * denominator. Reject impossible deadzones and overflowed ranges. */
  if (span <= 0 || span > INT_MAX || flat < 0 ||
      2LL * flat >= span)
    return false;
  parms->min = min;
  parms->max = max;
  parms->flat = flat;
  parms->avg = (int)(((int64_t)min + max) / 2);
  parms->range = max - parms->avg;
  parms->diff = (int)span;
  return true;
}

/* Explicit input= nodes and the selected-name udev startup sweep may refer
 * to the same event device. Compare the opened kernel node, not just its
 * eventN pathname (which can change across reconnects). */
static bool evdev_fd_already_registered(int fd) {
  struct stat candidate;
  if (fstat(fd, &candidate) < 0) return false;
  bool found = false;
  pthread_mutex_lock(&devices_mutex);
  for (int i = 0; i < numDevices; ++i) {
    struct stat existing;
    if (fstat(devices[i]->fd, &existing) == 0 &&
        existing.st_dev == candidate.st_dev &&
        existing.st_ino == candidate.st_ino &&
        existing.st_rdev == candidate.st_rdev) {
      found = true;
      break;
    }
  }
  pthread_mutex_unlock(&devices_mutex);
  return found;
}

static void evdev_remove(int devindex) {
  pthread_mutex_lock(&devices_mutex);
  if (devindex < 0 || devindex >= numDevices) {
    pthread_mutex_unlock(&devices_mutex);
    return;
  }
  struct input_device *device = devices[devindex];
  const short controller_id = device->controllerId;
  printf("Input device removed: %s (player %d)\n",
         libevdev_get_name(device->dev), controller_id + 1);
  if (controller_id >= 0 && controller_id < MAX_GAMEPADS)
    assignedControllerIds &= ~(1 << controller_id);
  if (device->is_gamepad && evdev_gamepads > 0)
    --evdev_gamepads;
  if (__atomic_load_n(&device->mouseEmulation, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&device->mouseEmulation, false, __ATOMIC_RELEASE);
    pthread_join(device->meThread, NULL);
  }
  libevdev_free(device->dev);
  loop_remove_fd(device->fd);
  close(device->fd);
  input_slots_remove(devices, &numDevices, devindex);
  free(device);
  pthread_mutex_unlock(&devices_mutex);
  /* Never call the streaming library while holding the device lifetime lock. */
  if (controller_id >= 0)
    LiSendMultiControllerEvent(controller_id, assignedControllerIds,
                               0, 0, 0, 0, 0, 0, 0);
}

/* Out-of-range or malformed axis geometry must never trigger integer
 * overflow or divide by zero in an input callback. */
static bool evdev_axis_usable(const struct input_abs_parms *parms) {
  if (!parms || parms->flat < 0) return false;
  const int64_t span = (int64_t)parms->max - parms->min;
  return span > 0 && span <= INT_MAX && 2LL * parms->flat < span;
}

static short evdev_convert_value(struct input_event *ev, struct input_device *dev,
                                 struct input_abs_parms *parms, bool reverse) {
  (void)dev;
  if (!evdev_axis_usable(parms)) return 0;
  const int64_t value = ev->value;
  const int64_t flat = parms->flat;
  if (llabs(value - (int64_t)parms->avg) < flat) return 0;
  if (value > parms->max) return reverse ? SHRT_MIN : SHRT_MAX;
  if (value < parms->min) return reverse ? SHRT_MAX : SHRT_MIN;
  const int64_t span = (int64_t)parms->max - parms->min;
  const int64_t numerator = reverse ?
      (int64_t)parms->max - (value < parms->avg ? flat * 2 : 0) - value :
      value - (value > parms->avg ? flat * 2 : 0) - parms->min;
  int64_t scaled = numerator * ((int64_t)SHRT_MAX - SHRT_MIN) /
                   (span - flat * 2) + SHRT_MIN;
  if (scaled > SHRT_MAX) scaled = SHRT_MAX;
  if (scaled < SHRT_MIN) scaled = SHRT_MIN;
  return (short)scaled;
}

static unsigned char evdev_convert_value_byte(struct input_event *ev,
                          struct input_device *dev, struct input_abs_parms *parms,
                          char halfaxis) {
  if (!evdev_axis_usable(parms)) return 0;
  if (halfaxis == 0) {
    const int64_t value = ev->value;
    const int64_t min = parms->min;
    const int64_t flat = parms->flat;
    if (value <= min + flat) return 0;
    if (value >= parms->max) return UCHAR_MAX;
    const int64_t denominator = (int64_t)parms->max - min - flat;
    int64_t scaled = (value - flat - min) * UCHAR_MAX / denominator;
    if (scaled < 0) scaled = 0;
    if (scaled > UCHAR_MAX) scaled = UCHAR_MAX;
    return (unsigned char)scaled;
  }
  const short val = evdev_convert_value(ev, dev, parms, false);
  if (halfaxis == '~')
    return (unsigned char)(UCHAR_MAX - evdev_convert_value_byte(ev, dev, parms, 0));
  if (halfaxis == '-' && val < 0)
    return (unsigned char)(-(int)val * UCHAR_MAX / -(int)SHRT_MIN);
  if (halfaxis == '+' && val > 0)
    return (unsigned char)((int)val * UCHAR_MAX / SHRT_MAX);
  return 0;
}

void *HandleMouseEmulation(void* param)
{
  struct input_device* dev = (struct input_device*) param;

  while (__atomic_load_n(&dev->mouseEmulation, __ATOMIC_ACQUIRE)) {
    usleep(MOUSE_EMULATION_POLLING_INTERVAL);

    short rawX;
    short rawY;

    // All four axes are published by the evdev thread.
    const short lx = __atomic_load_n(&dev->leftStickX, __ATOMIC_RELAXED);
    const short ly = __atomic_load_n(&dev->leftStickY, __ATOMIC_RELAXED);
    const short rx = __atomic_load_n(&dev->rightStickX, __ATOMIC_RELAXED);
    const short ry = __atomic_load_n(&dev->rightStickY, __ATOMIC_RELAXED);
    if ((uint32_t)abs(lx) + abs(ly) > (uint32_t)abs(rx) + abs(ry)) {
      rawX = lx;
      rawY = ly;
    } else {
      rawX = rx;
      rawY = ry;
    }

    float deltaX;
    float deltaY;

    // Produce a base vector for mouse movement with increased speed as we deviate further from center
    deltaX = pow((float)rawX / 32767.0f * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);
    deltaY = pow((float)rawY / 32767.0f * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);

    // Enforce deadzones
    deltaX = fabs(deltaX) > MOUSE_EMULATION_DEADZONE ? deltaX - MOUSE_EMULATION_DEADZONE : 0;
    deltaY = fabs(deltaY) > MOUSE_EMULATION_DEADZONE ? deltaY - MOUSE_EMULATION_DEADZONE : 0;

    if (deltaX != 0 || deltaY != 0)
      LiSendMouseMoveEvent(deltaX, -deltaY);
  }

  return NULL;
}

#define SET_BTN_FLAG(x, y) supportedButtonFlags |= (x >= 0) ? y : 0

static void send_controller_arrival(struct input_device *dev) {
  unsigned char type = LI_CTYPE_UNKNOWN;
  unsigned int supportedButtonFlags = 0;
  unsigned short capabilities = 0;

  switch (libevdev_get_id_vendor(dev->dev)) {
  case 0x045e: // Microsoft
    type = LI_CTYPE_XBOX;
    break;
  case 0x054c: // Sony
    type = LI_CTYPE_PS;
    break;
  case 0x057e: // Nintendo
    type = LI_CTYPE_NINTENDO;
    break;
  }

  const char* name = libevdev_get_name(dev->dev);
  if (name && type == LI_CTYPE_UNKNOWN) {

    // Try to guess based on the name
    if (strstr(name, "Xbox") || strstr(name, "X-Box") || strstr(name, "XBox") || strstr(name, "XBOX")) {
      type = LI_CTYPE_XBOX;
    }
  }

  SET_BTN_FLAG(dev->map->btn_a, A_FLAG);
  SET_BTN_FLAG(dev->map->btn_b, B_FLAG);
  SET_BTN_FLAG(dev->map->btn_x, X_FLAG);
  SET_BTN_FLAG(dev->map->btn_y, Y_FLAG);
  SET_BTN_FLAG(dev->map->btn_back, BACK_FLAG);
  SET_BTN_FLAG(dev->map->btn_start, PLAY_FLAG);
  SET_BTN_FLAG(dev->map->btn_guide, SPECIAL_FLAG);
  SET_BTN_FLAG(dev->map->btn_leftstick, LS_CLK_FLAG);
  SET_BTN_FLAG(dev->map->btn_rightstick, RS_CLK_FLAG);
  SET_BTN_FLAG(dev->map->btn_leftshoulder, LB_FLAG);
  SET_BTN_FLAG(dev->map->btn_rightshoulder, RB_FLAG);
  SET_BTN_FLAG(dev->map->btn_misc1, MISC_FLAG);
  SET_BTN_FLAG(dev->map->btn_paddle1, PADDLE1_FLAG);
  SET_BTN_FLAG(dev->map->btn_paddle2, PADDLE2_FLAG);
  SET_BTN_FLAG(dev->map->btn_paddle3, PADDLE3_FLAG);
  SET_BTN_FLAG(dev->map->btn_paddle4, PADDLE4_FLAG);
  SET_BTN_FLAG(dev->map->btn_touchpad, TOUCHPAD_FLAG);

  if (dev->map->abs_lefttrigger >= 0 && dev->map->abs_righttrigger >= 0)
    capabilities |= LI_CCAP_ANALOG_TRIGGERS;

  // TODO: Probe for this properly
  capabilities |= LI_CCAP_RUMBLE;

  LiSendControllerArrivalEvent(dev->controllerId, assignedControllerIds, type,
                               supportedButtonFlags, capabilities);
}

static bool evdev_handle_event(struct input_event *ev, struct input_device *dev) {
  bool gamepadModified = false;

  switch (ev->type) {
  case EV_SYN:
    if (ev->code != SYN_REPORT) break;
    if (dev->mouseDeltaX != 0 || dev->mouseDeltaY != 0) {
      switch (dev->rotate) {
      case 90:
        LiSendMouseMoveEvent(dev->mouseDeltaY, moonlight_negate_rel(dev->mouseDeltaX));
        break;
      case 180:
        LiSendMouseMoveEvent(moonlight_negate_rel(dev->mouseDeltaX),
                             moonlight_negate_rel(dev->mouseDeltaY));
        break;
      case 270:
        LiSendMouseMoveEvent(moonlight_negate_rel(dev->mouseDeltaY), dev->mouseDeltaX);
        break;
      default:
        LiSendMouseMoveEvent(dev->mouseDeltaX, dev->mouseDeltaY);
        break;
      }
      dev->mouseDeltaX = 0;
      dev->mouseDeltaY = 0;
    }
    if (dev->mouseVScroll != 0) {
      LiSendScrollEvent(dev->mouseVScroll);
      dev->mouseVScroll = 0;
    }
    if (dev->mouseHScroll != 0) {
      LiSendHScrollEvent(dev->mouseHScroll);
      dev->mouseHScroll = 0;
    }
    if (dev->gamepadModified) {
      if (dev->controllerId < 0) {
        pthread_mutex_lock(&devices_mutex);
        for (int i = 0; i < MAX_GAMEPADS; i++) {
          if ((assignedControllerIds & (1 << i)) == 0) {
            assignedControllerIds |= (1 << i);
            dev->controllerId = i;
            printf("Assigned %s as player %d\n", libevdev_get_name(dev->dev), i+1);
            break;
          }
        }
        //Use id 0 when too many gamepads are connected
        if (dev->controllerId < 0)
          dev->controllerId = 0;
        pthread_mutex_unlock(&devices_mutex);

        // Send controller arrival event to the host
        send_controller_arrival(dev);
      }
      // Send event only if mouse emulation is disabled.
      if (!__atomic_load_n(&dev->mouseEmulation, __ATOMIC_ACQUIRE))
        LiSendMultiControllerEvent(dev->controllerId, assignedControllerIds, dev->buttonFlags, dev->leftTrigger, dev->rightTrigger, dev->leftStickX, dev->leftStickY, dev->rightStickX, dev->rightStickY);
      dev->gamepadModified = false;
    }
    break;
  case EV_KEY:
    if (ev->code > KEY_MAX)
      return true;
#ifdef HAVE_LIBCEC
    /* Vortex routes local media keys to TV/AVR over libCEC. Do not send
     * these to Sunshine when an opted-in CEC adapter is actually open. */
    if (cec_handle_media_key(ev->code, ev->value))
      return true;
#endif

    /* A navigation remote's Back/Exit is local session navigation, not a
     * Windows key. Controllers and full keyboards intentionally keep their
     * existing host-facing Back/View and Escape behavior. */
    if (dev->is_remote && ev->value == 1 &&
        (ev->code == KEY_BACK || ev->code == KEY_EXIT || ev->code == KEY_ESC)) {
      printf("Remote Back/Exit requested local stream exit\n");
      return false;
    }
    if (ev->code < sizeof(keyCodes)/sizeof(keyCodes[0])) {
      char modifier = 0;
      switch (ev->code) {
      case KEY_LEFTSHIFT:
      case KEY_RIGHTSHIFT:
        modifier = MODIFIER_SHIFT;
        break;
      case KEY_LEFTALT:
      case KEY_RIGHTALT:
        modifier = MODIFIER_ALT;
        break;
      case KEY_LEFTCTRL:
      case KEY_RIGHTCTRL:
        modifier = MODIFIER_CTRL;
        break;
      case KEY_LEFTMETA:
      case KEY_RIGHTMETA:
        modifier = MODIFIER_META;
        break;
      }
      if (modifier != 0) {
        if (ev->value)
          dev->modifiers |= modifier;
        else
          dev->modifiers &= ~modifier;
      }

      // After the quit key combo is pressed, quit once all keys are raised
      if ((dev->modifiers & ACTION_MODIFIERS) == ACTION_MODIFIERS &&
          ev->code == QUIT_KEY && ev->value != 0) {
        waitingToExitOnModifiersUp = true;
        return true;
      } else if (waitingToExitOnModifiersUp && dev->modifiers == 0)
        return false;

      short code = 0x80 << 8 | keyCodes[ev->code];
      LiSendKeyboardEvent(code, ev->value?KEY_ACTION_DOWN:KEY_ACTION_UP, dev->modifiers);
    } else {
      int mouseCode = 0;
      int gamepadCode = 0;
      int index = dev->key_map[ev->code];

      switch (ev->code) {
      case BTN_LEFT:
        mouseCode = BUTTON_LEFT;
        break;
      case BTN_MIDDLE:
        mouseCode = BUTTON_MIDDLE;
        break;
      case BTN_RIGHT:
        mouseCode = BUTTON_RIGHT;
        break;
      case BTN_SIDE:
        mouseCode = BUTTON_X1;
        break;
      case BTN_EXTRA:
        mouseCode = BUTTON_X2;
        break;
      case BTN_TOUCH:
        if (ev->value == 1) {
          dev->touchDownTime.tv_sec = ev->input_event_sec;
          dev->touchDownTime.tv_usec = ev->input_event_usec;
        } else {
          if (dev->touchDownX != TOUCH_UP && dev->touchDownY != TOUCH_UP) {
            if (moonlight_touch_is_tap(dev->touchX, dev->touchY,
                                      dev->touchDownX, dev->touchDownY,
                                      TOUCH_CLICK_RADIUS)) {
              struct timeval eventTime;
              eventTime.tv_sec = ev->input_event_sec;
              eventTime.tv_usec = ev->input_event_usec;
              struct timeval elapsedTime;
              timersub(&eventTime, &dev->touchDownTime, &elapsedTime);
              int holdTimeMs = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
              int button = holdTimeMs >= TOUCH_RCLICK_TIME ? BUTTON_RIGHT : BUTTON_LEFT;
              LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, button);
              usleep(TOUCH_CLICK_DELAY);
              LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
            }
          }
          dev->touchDownX = TOUCH_UP;
          dev->touchDownY = TOUCH_UP;
        }
        break;
      default:
        gamepadModified = true;
        if (dev->map == NULL)
          break;
        else if (index == dev->map->btn_a)
          gamepadCode = A_FLAG;
        else if (index == dev->map->btn_x)
          gamepadCode = X_FLAG;
        else if (index == dev->map->btn_y)
          gamepadCode = Y_FLAG;
        else if (index == dev->map->btn_b)
          gamepadCode = B_FLAG;
        else if (index == dev->map->btn_dpup)
          gamepadCode = UP_FLAG;
        else if (index == dev->map->btn_dpdown)
          gamepadCode = DOWN_FLAG;
        else if (index == dev->map->btn_dpright)
          gamepadCode = RIGHT_FLAG;
        else if (index == dev->map->btn_dpleft)
          gamepadCode = LEFT_FLAG;
        else if (index == dev->map->btn_leftstick)
          gamepadCode = LS_CLK_FLAG;
        else if (index == dev->map->btn_rightstick)
          gamepadCode = RS_CLK_FLAG;
        else if (index == dev->map->btn_leftshoulder)
          gamepadCode = LB_FLAG;
        else if (index == dev->map->btn_rightshoulder)
          gamepadCode = RB_FLAG;
        else if (index == dev->map->btn_start)
          gamepadCode = PLAY_FLAG;
        else if (index == dev->map->btn_back)
          gamepadCode = BACK_FLAG;
        else if (index == dev->map->btn_guide)
          gamepadCode = SPECIAL_FLAG;
        else if (index == dev->map->btn_misc1)
          gamepadCode = MISC_FLAG;
        else if (index == dev->map->btn_paddle1)
          gamepadCode = PADDLE1_FLAG;
        else if (index == dev->map->btn_paddle2)
          gamepadCode = PADDLE2_FLAG;
        else if (index == dev->map->btn_paddle3)
          gamepadCode = PADDLE3_FLAG;
        else if (index == dev->map->btn_paddle4)
          gamepadCode = PADDLE4_FLAG;
        else if (index == dev->map->btn_touchpad)
          gamepadCode = TOUCHPAD_FLAG;
      }

      if (mouseCode != 0) {
        LiSendMouseButtonEvent(ev->value?BUTTON_ACTION_PRESS:BUTTON_ACTION_RELEASE, mouseCode);
        gamepadModified = false;
      } else if (gamepadCode != 0) {
        if (ev->value) {
          dev->buttonFlags |= gamepadCode;
          dev->btnDownTime.tv_sec = ev->input_event_sec;
          dev->btnDownTime.tv_usec = ev->input_event_usec;
        } else
          dev->buttonFlags &= ~gamepadCode;

        if (mouseEmulationEnabled && gamepadCode == PLAY_FLAG && ev->value == 0) {
          struct timeval eventTime;
          eventTime.tv_sec = ev->input_event_sec;
          eventTime.tv_usec = ev->input_event_usec;
          struct timeval elapsedTime;
          timersub(&eventTime, &dev->btnDownTime, &elapsedTime);
          int holdTimeMs = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
          if (holdTimeMs >= MOUSE_EMULATION_LONG_PRESS_TIME) {
            if (__atomic_load_n(&dev->mouseEmulation, __ATOMIC_ACQUIRE)) {
              __atomic_store_n(&dev->mouseEmulation, false, __ATOMIC_RELEASE);
              pthread_join(dev->meThread, NULL);
              dev->meThread = 0;
              printf("Mouse emulation disabled for controller %d.\n", dev->controllerId);
            } else {
              __atomic_store_n(&dev->mouseEmulation, true, __ATOMIC_RELEASE);
              if (pthread_create(&dev->meThread, NULL, HandleMouseEmulation, dev) != 0) {
                __atomic_store_n(&dev->mouseEmulation, false, __ATOMIC_RELEASE);
                fprintf(stderr, "Cannot start controller mouse-emulation thread\n");
              } else {
                printf("Mouse emulation enabled for controller %d.\n", dev->controllerId);
              }
            }
            // clear gamepad state.
            LiSendMultiControllerEvent(dev->controllerId, assignedControllerIds, 0, 0, 0, 0, 0, 0, 0);
          }
        } else if (__atomic_load_n(&dev->mouseEmulation, __ATOMIC_ACQUIRE)) {
          char action = ev->value ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE;
          switch (gamepadCode) {
            case A_FLAG:
              LiSendMouseButtonEvent(action, BUTTON_LEFT);
              break;
            case B_FLAG:
              LiSendMouseButtonEvent(action, BUTTON_RIGHT);
              break;
            case X_FLAG:
              LiSendMouseButtonEvent(action, BUTTON_MIDDLE);
              break;
            case LB_FLAG:
              LiSendMouseButtonEvent(action, BUTTON_X1);
              break;
            case RB_FLAG:
              LiSendMouseButtonEvent(action, BUTTON_X2);
              break;
          }
        }
      } else if (dev->map != NULL && index == dev->map->btn_lefttrigger)
        dev->leftTrigger = ev->value ? UCHAR_MAX : 0;
      else if (dev->map != NULL && index == dev->map->btn_righttrigger)
        dev->rightTrigger = ev->value ? UCHAR_MAX : 0;
      else {
        if (dev->map != NULL)
          fprintf(stderr, "Unmapped button: %d\n", ev->code);

        gamepadModified = false;
      }
    }
    break;
  case EV_REL:
    switch (ev->code) {
      case REL_X:
        dev->mouseDeltaX = moonlight_accumulate_rel(dev->mouseDeltaX, ev->value);
        break;
      case REL_Y:
        dev->mouseDeltaY = moonlight_accumulate_rel(dev->mouseDeltaY, ev->value);
        break;
      case REL_HWHEEL:
        dev->mouseHScroll = moonlight_accumulate_rel(dev->mouseHScroll, ev->value);
        break;
      case REL_WHEEL:
        dev->mouseVScroll = moonlight_accumulate_rel(dev->mouseVScroll, ev->value);
        break;
    }
    break;
  case EV_ABS:
    if (ev->code > ABS_MAX)
      return true;
    if (dev->is_touchscreen) {
      switch (ev->code) {
      case ABS_X:
        if (dev->touchDownX == TOUCH_UP) {
          dev->touchDownX = ev->value;
          dev->touchX = ev->value;
        } else {
          dev->mouseDeltaX = moonlight_accumulate_rel(dev->mouseDeltaX,
              moonlight_touch_delta(ev->value, dev->touchX));
          dev->touchX = ev->value;
        }
        break;
      case ABS_Y:
        if (dev->touchDownY == TOUCH_UP) {
          dev->touchDownY = ev->value;
          dev->touchY = ev->value;
        } else {
          dev->mouseDeltaY = moonlight_accumulate_rel(dev->mouseDeltaY,
              moonlight_touch_delta(ev->value, dev->touchY));
          dev->touchY = ev->value;
        }
        break;
      }
      break;
    }

    if (dev->map == NULL)
      break;

    gamepadModified = true;
    int index = dev->abs_map[ev->code];
    int hat_index = (ev->code - ABS_HAT0X) / 2;
    int hat_dir_index = (ev->code - ABS_HAT0X) % 2;

    switch (ev->code) {
    case ABS_HAT0X:
    case ABS_HAT0Y:
    case ABS_HAT1X:
    case ABS_HAT1Y:
    case ABS_HAT2X:
    case ABS_HAT2Y:
    case ABS_HAT3X:
    case ABS_HAT3Y:
      dev->hats_state[hat_index][hat_dir_index] = ev->value < 0 ? -1 : (ev->value == 0 ? 0 : 1);
      int hat_state = hat_constants[dev->hats_state[hat_index][1] + 1][dev->hats_state[hat_index][0] + 1];
      if (hat_index == dev->map->hat_dpup)
        set_hat(dev->buttonFlags, UP_FLAG, hat_state, dev->map->hat_dir_dpup);
      if (hat_index == dev->map->hat_dpdown)
        set_hat(dev->buttonFlags, DOWN_FLAG, hat_state, dev->map->hat_dir_dpdown);
      if (hat_index == dev->map->hat_dpright)
        set_hat(dev->buttonFlags, RIGHT_FLAG, hat_state, dev->map->hat_dir_dpright);
      if (hat_index == dev->map->hat_dpleft)
        set_hat(dev->buttonFlags, LEFT_FLAG, hat_state, dev->map->hat_dir_dpleft);
      break;
    default:
      if (index == dev->map->abs_leftx)
        __atomic_store_n(&dev->leftStickX, evdev_convert_value(ev, dev, &dev->xParms, dev->map->reverse_leftx), __ATOMIC_RELAXED);
      else if (index == dev->map->abs_lefty)
        __atomic_store_n(&dev->leftStickY, evdev_convert_value(ev, dev, &dev->yParms, !dev->map->reverse_lefty), __ATOMIC_RELAXED);
      else if (index == dev->map->abs_rightx)
        __atomic_store_n(&dev->rightStickX, evdev_convert_value(ev, dev, &dev->rxParms, dev->map->reverse_rightx), __ATOMIC_RELAXED);
      else if (index == dev->map->abs_righty)
        __atomic_store_n(&dev->rightStickY, evdev_convert_value(ev, dev, &dev->ryParms, !dev->map->reverse_righty), __ATOMIC_RELAXED);
      else
        gamepadModified = false;

      if (index == dev->map->abs_lefttrigger) {
        dev->leftTrigger = evdev_convert_value_byte(ev, dev, &dev->zParms, dev->map->halfaxis_lefttrigger);
        gamepadModified = true;
      }
      if (index == dev->map->abs_righttrigger) {
        dev->rightTrigger = evdev_convert_value_byte(ev, dev, &dev->rzParms, dev->map->halfaxis_righttrigger);
        gamepadModified = true;
      }

      if (index == dev->map->abs_dpright) {
        if (evdev_convert_value_byte(ev, dev, &dev->rightParms, dev->map->halfaxis_dpright) > 127)
          dev->buttonFlags |= RIGHT_FLAG;
        else
          dev->buttonFlags &= ~RIGHT_FLAG;

        gamepadModified = true;
      }
      if (index == dev->map->abs_dpleft) {
        if (evdev_convert_value_byte(ev, dev, &dev->leftParms, dev->map->halfaxis_dpleft) > 127)
          dev->buttonFlags |= LEFT_FLAG;
        else
          dev->buttonFlags &= ~LEFT_FLAG;

        gamepadModified = true;
      }
      if (index == dev->map->abs_dpup) {
        if (evdev_convert_value_byte(ev, dev, &dev->upParms, dev->map->halfaxis_dpup) > 127)
          dev->buttonFlags |= UP_FLAG;
        else
          dev->buttonFlags &= ~UP_FLAG;

        gamepadModified = true;
      }
      if (index == dev->map->abs_dpdown) {
        if (evdev_convert_value_byte(ev, dev, &dev->downParms, dev->map->halfaxis_dpdown) > 127)
          dev->buttonFlags |= DOWN_FLAG;
        else
          dev->buttonFlags &= ~DOWN_FLAG;

        gamepadModified = true;
      }
    }
  }

  if (gamepadModified && (dev->buttonFlags & QUIT_BUTTONS) == QUIT_BUTTONS) {
    LiSendMultiControllerEvent(dev->controllerId, assignedControllerIds, 0, 0, 0, 0, 0, 0, 0);
    return false;
  }

  dev->gamepadModified |= gamepadModified;
  return true;
}

/* The evdev state includes the current ABS value even before the first
 * event reaches the wizard. Capture the inactive position BEFORE prompting. */
static void evdev_snapshot_mapping_rest(void) {
  for (int d = 0; d < numDevices; ++d) {
    for (unsigned int axis = 0; axis < ABS_CNT; ++axis) {
      if (libevdev_has_event_code(devices[d]->dev, EV_ABS, axis))
        devices[d]->mapping_rest[axis] =
            libevdev_get_event_value(devices[d]->dev, EV_ABS, axis);
    }
  }
}

static bool evdev_handle_mapping_event(struct input_event *ev, struct input_device *dev) {
  int index, hat_index;
  switch (ev->type) {
  case EV_KEY:
    if (ev->code > KEY_MAX) break;
    index = dev->key_map[ev->code];
    if (currentKey != NULL) {
      if (ev->value)
        *currentKey = index;
      else if (*currentKey != -1 && index == *currentKey)
        return false;
    }
    break;
  case EV_ABS:
    if (ev->code > ABS_MAX) break;
    /* Division of a negative code can round toward zero, creating a
     * bogus hat 0 with a negative direction index. */
    hat_index = -1;
    if (ev->code >= ABS_HAT0X && ev->code <= ABS_HAT3Y) {
      hat_index = (ev->code - ABS_HAT0X) / 2;
      int hat_dir_index = (ev->code - ABS_HAT0X) % 2;
      dev->hats_state[hat_index][hat_dir_index] =
          ev->value < 0 ? -1 : (ev->value == 0 ? 0 : 1);
    }
    if (currentAbs != NULL) {
      /* The mapping helper expects a mapped axis index, not Linux's ABS
       * event code. Missing indices cannot leave 'parms' uninitialized. */
      const int mapped = dev->abs_map[ev->code];
      if (mapped < 0) break;
      struct input_abs_parms parms = {0};
      if (!evdev_init_parms(dev, &parms, mapped)) break;
      if (*currentAbs >= 0) {
        /* Once a physical axis is captured, ignore unrelated events and
         * opposite-edge readings. A 0..255 trigger rests at an endpoint,
         * unlike a centered stick. */
        if (mapped == *currentAbs &&
            moonlight_mapping_axis_released(
                ev->value, dev->mapping_rest[ev->code],
                parms.min, parms.max))
          return false;
        break;
      }
      if (moonlight_mapping_axis_pressed(
              ev->value, dev->mapping_rest[ev->code],
              parms.min, parms.max)) {
        const bool positive = ev->value > dev->mapping_rest[ev->code];
        *currentAbs = mapped;
        if (currentReverse) *currentReverse = !positive;
        if (currentHalfAxis) {
          /* Full 0..255 scaling for a trigger pressed from its minimum.
           * A reverse/unipolar trigger or bipolar axis needs a half-axis. */
          *currentHalfAxis = moonlight_mapping_trigger_mode(
              parms.min, parms.max, dev->mapping_rest[ev->code], positive);
        }
      }
    } else if (currentHat != NULL && hat_index >= 0 && hat_index < 4) {
      const int direction =
          hat_constants[dev->hats_state[hat_index][1] + 1]
                       [dev->hats_state[hat_index][0] + 1];
      if (*currentHat >= 0) {
        /* Finish only after the SAME hat returns to neutral. Never record
         * a zero direction, which would be permanently pressed at runtime. */
        if (hat_index == *currentHat && direction == 0)
          return false;
      } else if (moonlight_mapping_hat_pressed(direction)) {
        *currentHat = hat_index;
        *currentHatDir = direction;
      }
    }
    break;
  }
  return true;
}

static void evdev_drain(void) {
  for (int i = 0; i < numDevices; i++) {
    struct input_event ev;
    bool syncing = false;
    for (;;) {
      int rc = libevdev_next_event(devices[i]->dev,
              syncing ? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL, &ev);
      if (rc == LIBEVDEV_READ_STATUS_SYNC) { syncing = true; continue; }
      if (rc == -EAGAIN && syncing) { syncing = false; continue; }
      if (rc < 0) break;
    }
  }
}

static int evdev_handle(int fd) {
  for (int i = 0; i < numDevices; ++i) {
    if (devices[i]->fd != fd) continue;
    bool syncing = false;
    for (;;) {
      struct input_event ev;
      int rc = libevdev_next_event(devices[i]->dev,
          syncing ? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL, &ev);
      if (rc == LIBEVDEV_READ_STATUS_SYNC) {
        if (!syncing) {
          fprintf(stderr, "Input events dropped; resynchronizing device %d\n", fd);
          syncing = true;
          continue; /* Initial SYN_DROPPED is not an input event. */
        }
        /* libevdev supplies current button/axis state while in SYNC mode. */
        if (!handler(&ev, devices[i])) return LOOP_RETURN;
        continue;
      }
      if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
        if (!handler(&ev, devices[i])) return LOOP_RETURN;
        continue;
      }
      if (rc == -EAGAIN && syncing) {
        syncing = false;
        continue; /* Re-read normal events after sync is complete. */
      }
      if (rc == -EINTR) continue;
      if (rc == -ENODEV) {
        evdev_remove(i);
        return LOOP_OK;
      }
      if (rc != -EAGAIN) {
        fprintf(stderr, "Input device %d read failed: %s\n", fd, strerror(-rc));
        return LOOP_ERROR;
      }
      break;
    }
    break;
  }
  return LOOP_OK;
}

/* A readable path is not necessarily an evdev device. Validate libevdev
 * before using metadata or registering a poll fd. Failed initialization
 * closes the fd; callers must not close it a second time. */
static struct libevdev *evdev_open_checked(int fd, const char *path) {
  struct libevdev *dev = libevdev_new();
  if (!dev) {
    fprintf(stderr, "Cannot allocate libevdev for %s\n", path);
    close(fd);
    return NULL;
  }
  int rc = libevdev_set_fd(dev, fd);
  if (rc < 0) {
    fprintf(stderr, "Not a usable evdev device %s: %s\n",
            path, strerror(-rc));
    libevdev_free(dev);
    close(fd);
    return NULL;
  }
  return dev;
}

void evdev_create(const char* device, struct mapping* mappings, bool verbose, int rotate) {
  int fd = open(device, O_RDWR|O_NONBLOCK);
  int error = fd < 0 ? errno : 0;
  /* Limited, non-mutating recovery for transient evdev ownership changes.
   * 3 retries at 150 ms; never unbind HID/xpad or touch a keyboard driver. */
  if (fd < 0 && input_handoff_enabled("MOONLIGHT_INPUT_RECOVERY")) {
    for (unsigned attempt = 0;
         fd < 0 && input_handoff_retryable(error, attempt); attempt++) {
      usleep(150000);
      fd = open(device, O_RDWR|O_NONBLOCK);
      error = fd < 0 ? errno : 0;
    }
  }
  if (fd < 0) {
    fprintf(stderr, "Failed to open device %s: %s\n",
            device, strerror(error));
    fflush(stderr);
    return;
  }
  if (evdev_fd_already_registered(fd)) {
    if (verbose)
      fprintf(stderr, "Skipping already registered input device %s\n", device);
    close(fd);
    return;
  }

  struct libevdev *evdev = evdev_open_checked(fd, device);
  if (!evdev)
    return;
  const char* name = libevdev_get_name(evdev);
  if (!name)
    name = "Unknown input device";

  /* The event number may have been recycled since Luna's snapshot.
   * Validate the opened fd, including explicit input= paths, not just
   * nodes discovered later through udev. */
  const char *selectors = getenv("MOONLIGHT_INPUT_SELECTORS_V2");
  const char *legacy_names = getenv("MOONLIGHT_INPUT_NAMES_HEX");
  if (selectors || legacy_names) {
    char uniq[256] = {0}, phys[256] = {0};
    int n = ioctl(fd, EVIOCGUNIQ(sizeof(uniq)), uniq);
    if (n <= 0 || n >= (int)sizeof(uniq)) uniq[0] = 0;
    n = ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
    if (n <= 0 || n >= (int)sizeof(phys)) phys[0] = 0;
    uniq[sizeof(uniq) - 1] = 0;
    phys[sizeof(phys) - 1] = 0;
    if (!moonlight_selected_identity(selectors, legacy_names, name, uniq, phys)) {
      if (verbose)
        fprintf(stderr, "Ignoring unselected evdev device %s on %s\n", name, device);
      libevdev_free(evdev);
      close(fd);
      return;
    }
  }

  int16_t guid[8] = {0};
  guid[0] = int16_to_le(libevdev_get_id_bustype(evdev));
  int16_t vendor = libevdev_get_id_vendor(evdev);
  int16_t product = libevdev_get_id_product(evdev);
  if (vendor && product) {
    guid[2] = int16_to_le(vendor);
    guid[4] = int16_to_le(product);
    guid[6] = int16_to_le(libevdev_get_id_version(evdev));
  } else
    strncpy((char*) &guid[2], name, 11);

  char str_guid[33];
  char* buf = str_guid;
  for (int i = 0; i < 16; i++)
    buf += sprintf(buf, "%02x", ((unsigned char*) guid)[i]);

  struct mapping* default_mapping = NULL;
  struct mapping* xwc_mapping = NULL;
  while (mappings != NULL) {
    if (strncmp(str_guid, mappings->guid, 32) == 0) {
      if (verbose)
        printf("Detected %s (%s) on %s as %s\n", name, str_guid, device, mappings->name);

      break;
    } else if (strncmp("default", mappings->guid, 32) == 0)
      default_mapping = mappings;
    else if (strncmp("xwc", mappings->guid, 32) == 0)
      xwc_mapping = mappings;

    mappings = mappings->next;
  }

  if (mappings == NULL && strstr(name, "Xbox 360 Wireless Receiver") != NULL)
    mappings = xwc_mapping;

  bool is_keyboard = libevdev_has_event_code(evdev, EV_KEY, KEY_Q);
  bool is_mouse = libevdev_has_event_type(evdev, EV_REL) || libevdev_has_event_code(evdev, EV_KEY, BTN_LEFT);
  bool is_touchscreen = libevdev_has_event_code(evdev, EV_KEY, BTN_TOUCH);

  // This classification logic comes from SDL
  bool is_accelerometer =
    ((libevdev_has_event_code(evdev, EV_ABS, ABS_X) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_Y) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_Z)) ||
     (libevdev_has_event_code(evdev, EV_ABS, ABS_RX) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_RY) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_RZ))) &&
    !libevdev_has_event_type(evdev, EV_KEY);
  bool is_gamepad =
    ((libevdev_has_event_code(evdev, EV_ABS, ABS_X) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_Y)) ||
     (libevdev_has_event_code(evdev, EV_ABS, ABS_HAT0X) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_HAT0Y))) &&
    (libevdev_has_event_code(evdev, EV_KEY, BTN_TRIGGER) ||
     libevdev_has_event_code(evdev, EV_KEY, BTN_A) ||
     libevdev_has_event_code(evdev, EV_KEY, BTN_1) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_RX) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_RY) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_RZ) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_THROTTLE) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_RUDDER) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_WHEEL) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_GAS) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_BRAKE));

  const bool has_remote_back =
      libevdev_has_event_code(evdev, EV_KEY, KEY_BACK) ||
      libevdev_has_event_code(evdev, EV_KEY, KEY_EXIT) ||
      libevdev_has_event_code(evdev, EV_KEY, KEY_ESC);
  const bool has_remote_direction =
      libevdev_has_event_code(evdev, EV_KEY, KEY_UP) ||
      libevdev_has_event_code(evdev, EV_KEY, KEY_DOWN) ||
      libevdev_has_event_code(evdev, EV_KEY, KEY_LEFT) ||
      libevdev_has_event_code(evdev, EV_KEY, KEY_RIGHT);
  const bool has_remote_enter =
      libevdev_has_event_code(evdev, EV_KEY, KEY_ENTER) ||
      libevdev_has_event_code(evdev, EV_KEY, KEY_OK);
  const bool is_remote =
      moonlight_is_known_navigation_remote(name) ||
      moonlight_is_navigation_remote(
          is_keyboard, is_gamepad, has_remote_back,
          has_remote_direction, has_remote_enter);

  if (is_accelerometer) {
    if (verbose)
      printf("Ignoring accelerometer: %s\n", name);
    libevdev_free(evdev);
    close(fd);
    return;
  }

  if (is_gamepad) {
    if (mappings == NULL) {
      fprintf(stderr, "No mapping available for %s (%s) on %s\n", name, str_guid, device);
      mappings = default_mapping;
    }
  } else {
    if (verbose)
      printf("Not mapping %s as a gamepad\n", name);
    mappings = NULL;
  }

  struct input_device *new_device = calloc(1, sizeof(*new_device));
  pthread_mutex_lock(&devices_mutex);
  if (!new_device || !input_slots_append(&devices, &numDevices, new_device)) {
    pthread_mutex_unlock(&devices_mutex);
    fprintf(stderr, "Cannot allocate an input device slot\n");
    free(new_device);
    libevdev_free(evdev);
    close(fd);
    return;
  }
  const int dev = numDevices - 1;
  devices[dev]->fd = fd;
  devices[dev]->dev = evdev;
  devices[dev]->map = mappings;
  /* Set unused evdev indices to -2 to avoid aliasing with the default -1 in our mappings */
  memset(devices[dev]->key_map, -2, sizeof(devices[dev]->key_map));
  memset(devices[dev]->abs_map, -2, sizeof(devices[dev]->abs_map));
  devices[dev]->is_keyboard = is_keyboard;
  devices[dev]->is_mouse = is_mouse;
  devices[dev]->is_touchscreen = is_touchscreen;
  devices[dev]->is_remote = is_remote;
  /* An unrecognized controller without a mapping cannot send usable
   * gamepad events and must not inflate Sunshine's launch bitmap. */
  devices[dev]->is_gamepad = is_gamepad && mappings != NULL;
  devices[dev]->rotate = rotate;
  devices[dev]->touchDownX = TOUCH_UP;
  devices[dev]->touchDownY = TOUCH_UP;

  int nbuttons = 0;
  /* Count joystick buttons first like SDL does */
  for (int i = BTN_JOYSTICK; i < KEY_MAX; ++i) {
    if (libevdev_has_event_code(devices[dev]->dev, EV_KEY, i))
      devices[dev]->key_map[i] = nbuttons++;
  }
  for (int i = 0; i < BTN_JOYSTICK; ++i) {
    if (libevdev_has_event_code(devices[dev]->dev, EV_KEY, i))
      devices[dev]->key_map[i] = nbuttons++;
  }

  int naxes = 0;
  for (int i = 0; i < ABS_MAX; ++i) {
    /* Skip hats */
    if (i == ABS_HAT0X)
      i = ABS_HAT3Y;
    else if (libevdev_has_event_code(devices[dev]->dev, EV_ABS, i))
      devices[dev]->abs_map[i] = naxes++;
  }

  devices[dev]->controllerId = -1;
  devices[dev]->haptic_effect_id = -1;

  if (devices[dev]->map != NULL) {
    bool valid = evdev_init_parms(devices[dev], &(devices[dev]->xParms), devices[dev]->map->abs_leftx);
    valid &= evdev_init_parms(devices[dev], &(devices[dev]->yParms), devices[dev]->map->abs_lefty);
    valid &= evdev_init_parms(devices[dev], &(devices[dev]->zParms), devices[dev]->map->abs_lefttrigger);
    valid &= evdev_init_parms(devices[dev], &(devices[dev]->rxParms), devices[dev]->map->abs_rightx);
    valid &= evdev_init_parms(devices[dev], &(devices[dev]->ryParms), devices[dev]->map->abs_righty);
    valid &= evdev_init_parms(devices[dev], &(devices[dev]->rzParms), devices[dev]->map->abs_righttrigger);
    valid &= evdev_init_parms(devices[dev], &(devices[dev]->leftParms), devices[dev]->map->abs_dpleft);
    valid &= evdev_init_parms(devices[dev], &(devices[dev]->rightParms), devices[dev]->map->abs_dpright);
    valid &= evdev_init_parms(devices[dev], &(devices[dev]->upParms), devices[dev]->map->abs_dpup);
    valid &= evdev_init_parms(devices[dev], &(devices[dev]->downParms), devices[dev]->map->abs_dpdown);
    if (!valid) {
      fprintf(stderr, "Mapping for %s (%s) on %s is incorrect\n", name, str_guid, device);
      devices[dev]->map = NULL;
      devices[dev]->is_gamepad = false;
    }
  }

  if (devices[dev]->is_gamepad) {
    unsigned int hats_mask = 0;
    for (unsigned int h = 0; h < 4; ++h) {
      if (libevdev_has_event_code(devices[dev]->dev, EV_ABS, ABS_HAT0X + 2 * h) ||
          libevdev_has_event_code(devices[dev]->dev, EV_ABS, ABS_HAT0Y + 2 * h))
        hats_mask |= 1U << h;
    }
    if (!moonlight_mapping_has_usable_bindings(devices[dev]->map,
                                               nbuttons, naxes, hats_mask)) {
      fprintf(stderr, "Gamepad %s has no usable mapped controls; not advertising it\n", name);
      devices[dev]->map = NULL;
      devices[dev]->is_gamepad = false;
    }
  }

  if (grabbingDevices && (is_keyboard || is_mouse || is_touchscreen)) {
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
      fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);
    }
  }

  loop_add_fd(devices[dev]->fd, &evdev_handle, POLLIN);
  /* Count only successfully registered devices. Failed allocations must
   * never advertise a nonexistent controller in Sunshine's launch mask. */
  if (devices[dev]->is_gamepad)
    ++evdev_gamepads;
  pthread_mutex_unlock(&devices_mutex);
}

static bool evdev_map_key(char* keyName, short* key) {
  printf("Press %s\n", keyName);
  currentKey = key;
  currentHat = NULL;
  currentHatDir = NULL;
  currentAbs = NULL;
  currentReverse = NULL;
  currentHalfAxis = NULL;
  *key = -1;
  if (loop_main() != LOOP_OK) {
    fprintf(stderr, "Controller mapping interrupted by input I/O error\n");
    return false;
  }
  usleep(250000);
  evdev_drain();
  return *key >= 0;
}

static bool evdev_map_abs(char* keyName, short* abs, bool* reverse) {
  printf("Move %s\n", keyName);
  currentKey = NULL;
  currentHat = NULL;
  currentHatDir = NULL;
  currentAbs = abs;
  currentReverse = reverse;
  currentHalfAxis = NULL;
  evdev_snapshot_mapping_rest();
  *abs = -1;
  if (loop_main() != LOOP_OK) {
    fprintf(stderr, "Controller mapping interrupted by input I/O error\n");
    return false;
  }
  usleep(250000);
  evdev_drain();
  return *abs >= 0;
}

static bool evdev_map_hatkey(char* keyName, short* hat, short* hat_dir, short* key) {
  printf("Press %s\n", keyName);
  currentKey = key;
  currentHat = hat;
  currentHatDir = hat_dir;
  currentAbs = NULL;
  currentReverse = NULL;
  currentHalfAxis = NULL;
  *key = -1;
  *hat = -1;
  *hat_dir = -1;
  if (loop_main() != LOOP_OK) {
    fprintf(stderr, "Controller mapping interrupted by input I/O error\n");
    return false;
  }
  usleep(250000);
  evdev_drain();
  return (*hat >= 0 && moonlight_mapping_hat_pressed(*hat_dir)) ||
         *key >= 0;
}

static bool evdev_map_abskey(char* keyName, short* abs, short* key, char* halfaxis) {
  printf("Press %s\n", keyName);
  currentKey = key;
  currentHat = NULL;
  currentHatDir = NULL;
  currentAbs = abs;
  currentReverse = NULL;
  currentHalfAxis = halfaxis;
  evdev_snapshot_mapping_rest();
  *key = -1;
  *abs = -1;
  *halfaxis = 0;
  if (loop_main() != LOOP_OK) {
    fprintf(stderr, "Controller mapping interrupted by input I/O error\n");
    return false;
  }
  usleep(250000);
  evdev_drain();
  return *abs >= 0 || *key >= 0;
}

bool evdev_map(char* device) {
  /* The device must already be registered for event-loop mapping.
   * If evdev_create failed, waiting here would hang indefinitely. */
  if (numDevices == 0) {
    fprintf(stderr, "Controller mapping aborted: no registered input device\n");
    return false;
  }
  int fd = open(device, O_RDONLY|O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "Cannot open input mapping device %s: %s\n",
            device, strerror(errno));
    return false;
  }
  struct libevdev *evdev = evdev_open_checked(fd, device);
  if (!evdev)
    return false;
  const char* name = libevdev_get_name(evdev);
  if (!name)
    name = "Unknown input device";

  int16_t guid[8] = {0};
  guid[0] = int16_to_le(libevdev_get_id_bustype(evdev));
  guid[2] = int16_to_le(libevdev_get_id_vendor(evdev));
  guid[4] = int16_to_le(libevdev_get_id_product(evdev));
  guid[6] = int16_to_le(libevdev_get_id_version(evdev));
  char str_guid[33];
  char* buf = str_guid;
  for (int i = 0; i < 16; i++)
    buf += sprintf(buf, "%02x", ((unsigned char*) guid)[i]);

  struct mapping map = {0};
  /* The mapping parser's sentinel is -1, not zero. Leaving optional
   * controls at zero would serialize them as fictitious b0/a0 mappings. */
  memset(&map.abs_leftx, 0xff,
         offsetof(struct mapping, next) - offsetof(struct mapping, abs_leftx));
  strncpy(map.name, name, sizeof(map.name) - 1);
  strncpy(map.guid, str_guid, sizeof(map.guid) - 1);

  libevdev_free(evdev);
  close(fd);

  handler = evdev_handle_mapping_event;

#define MAP_STEP(call) do { \
  if (!(call)) { \
    fprintf(stderr, "Controller mapping failed; partial configuration discarded\n"); \
    return false; \
  } \
} while (0)
  MAP_STEP(evdev_map_abs("Left Stick Right", &(map.abs_leftx), &(map.reverse_leftx)));
  MAP_STEP(evdev_map_abs("Left Stick Up", &(map.abs_lefty), &(map.reverse_lefty)));
  MAP_STEP(evdev_map_key("Left Stick Button", &(map.btn_leftstick)));

  MAP_STEP(evdev_map_abs("Right Stick Right", &(map.abs_rightx), &(map.reverse_rightx)));
  MAP_STEP(evdev_map_abs("Right Stick Up", &(map.abs_righty), &(map.reverse_righty)));
  MAP_STEP(evdev_map_key("Right Stick Button", &(map.btn_rightstick)));

  MAP_STEP(evdev_map_hatkey("D-Pad Right", &(map.hat_dpright), &(map.hat_dir_dpright), &(map.btn_dpright)));
  MAP_STEP(evdev_map_hatkey("D-Pad Left", &(map.hat_dpleft), &(map.hat_dir_dpleft), &(map.btn_dpleft)));
  MAP_STEP(evdev_map_hatkey("D-Pad Up", &(map.hat_dpup), &(map.hat_dir_dpup), &(map.btn_dpup)));
  MAP_STEP(evdev_map_hatkey("D-Pad Down", &(map.hat_dpdown), &(map.hat_dir_dpdown), &(map.btn_dpdown)));

  MAP_STEP(evdev_map_key("Button X (1)", &(map.btn_x)));
  MAP_STEP(evdev_map_key("Button A (2)", &(map.btn_a)));
  MAP_STEP(evdev_map_key("Button B (3)", &(map.btn_b)));
  MAP_STEP(evdev_map_key("Button Y (4)", &(map.btn_y)));
  MAP_STEP(evdev_map_key("Back Button", &(map.btn_back)));
  MAP_STEP(evdev_map_key("Start Button", &(map.btn_start)));
  MAP_STEP(evdev_map_key("Special Button", &(map.btn_guide)));

  MAP_STEP(evdev_map_abskey("Left Trigger", &(map.abs_lefttrigger),
                            &(map.btn_lefttrigger), &(map.halfaxis_lefttrigger)));
  MAP_STEP(evdev_map_abskey("Right Trigger", &(map.abs_righttrigger),
                            &(map.btn_righttrigger), &(map.halfaxis_righttrigger)));

  MAP_STEP(evdev_map_key("Left Bumper", &(map.btn_leftshoulder)));
  MAP_STEP(evdev_map_key("Right Bumper", &(map.btn_rightshoulder)));
  mapping_print(&map);
#undef MAP_STEP
  return true;
}

void evdev_start() {
  // After grabbing, the only way to quit via the keyboard
  // is via the special key combo that the input handling
  // code looks for. For this reason, we wait to grab until
  // we're ready to take input events. Ctrl+C works up until
  // this point.
  for (int i = 0; i < numDevices; i++) {
    if ((devices[i]->is_keyboard || devices[i]->is_mouse || devices[i]->is_touchscreen) && ioctl(devices[i]->fd, EVIOCGRAB, 1) < 0) {
      fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);
    }
  }

  // Any new input devices detected after this point will be grabbed immediately
  grabbingDevices = true;

  // Handle input events until the quit combo is pressed
}

void evdev_stop() {
  /* A mouse-emulation thread may already be inside LiSendMouseMoveEvent.
   * Join it before LiStopConnection destroys the common-c input locks. */
  pthread_mutex_lock(&devices_mutex);
  grabbingDevices = false;
  for (int i = 0; i < numDevices; i++) {
    struct input_device *device = devices[i];
    if (__atomic_load_n(&device->mouseEmulation, __ATOMIC_ACQUIRE)) {
      __atomic_store_n(&device->mouseEmulation, false, __ATOMIC_RELEASE);
      pthread_join(device->meThread, NULL);
      device->meThread = 0;
    }
  }
  pthread_mutex_unlock(&devices_mutex);
  evdev_drain();
}

void evdev_init(bool mouse_emulation_enabled) {
  handler = evdev_handle_event;
  mouseEmulationEnabled = mouse_emulation_enabled;
}

/* Caller must hold devices_mutex throughout use of the returned pointer. */
static struct input_device* evdev_get_input_device(unsigned short controller_id) {
  for (int i=0; i<numDevices; i++)
    if (devices[i]->controllerId == controller_id)
      return devices[i];

  return NULL;
}

void evdev_rumble(unsigned short controller_id, unsigned short low_freq_motor, unsigned short high_freq_motor) {
  pthread_mutex_lock(&devices_mutex);
  struct input_device *device = evdev_get_input_device(controller_id);
  if (!device) {
    pthread_mutex_unlock(&devices_mutex);
    return;
  }

  if (device->haptic_effect_id >= 0) {
    ioctl(device->fd, EVIOCRMFF, device->haptic_effect_id);
    device->haptic_effect_id = -1;
  }

  if (low_freq_motor == 0 && high_freq_motor == 0) {
    pthread_mutex_unlock(&devices_mutex);
    return;
  }

  struct ff_effect effect = {0};
  effect.type = FF_RUMBLE;
  effect.id = -1;
  effect.replay.length = USHRT_MAX;
  effect.u.rumble.strong_magnitude = low_freq_motor;
  effect.u.rumble.weak_magnitude = high_freq_motor;
  if (ioctl(device->fd, EVIOCSFF, &effect) == -1) {
    pthread_mutex_unlock(&devices_mutex);
    return;
  }

  struct input_event event = {0};
  event.type = EV_FF;
  event.code = effect.id;
  event.value = 1;
  write(device->fd, (const void*) &event, sizeof(event));
  device->haptic_effect_id = effect.id;
  pthread_mutex_unlock(&devices_mutex);
}
