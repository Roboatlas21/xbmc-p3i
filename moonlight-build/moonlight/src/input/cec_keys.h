/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_CEC_KEYS_H
#define MOONLIGHT_CEC_KEYS_H

/* Linux EV_KEY media codes; distinct from HDMI-CEC User Control codes.
 * Vortex forwards these local remote keys to libCEC (TV/AVR), not Sunshine. */
enum MoonlightCecMediaAction {
  MOONLIGHT_CEC_MEDIA_NONE = 0,
  MOONLIGHT_CEC_MEDIA_MUTE,
  MOONLIGHT_CEC_MEDIA_VOLUME_DOWN,
  MOONLIGHT_CEC_MEDIA_VOLUME_UP
};

static inline enum MoonlightCecMediaAction
moonlight_cec_media_action(unsigned int linux_key) {
  switch (linux_key) {
    case 113: return MOONLIGHT_CEC_MEDIA_MUTE; /* KEY_MUTE */
    case 114: return MOONLIGHT_CEC_MEDIA_VOLUME_DOWN; /* KEY_VOLUMEDOWN */
    case 115: return MOONLIGHT_CEC_MEDIA_VOLUME_UP; /* KEY_VOLUMEUP */
    default: return MOONLIGHT_CEC_MEDIA_NONE;
  }
}

#endif
