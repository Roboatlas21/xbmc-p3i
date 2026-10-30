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

#include "audio.h"
#include "pulse_mapping.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus_multistream.h>
#include <pulse/simple.h>
#include <pulse/error.h>

static OpusMSDecoder* decoder;
static pa_simple *dev = NULL;
static short* pcmBuffer;
static int samplesPerFrame;
static int channelCount;

static void pulse_renderer_cleanup(void);

bool audio_pulse_init(char* audio_device) {
  pa_sample_spec spec = {
    .format = PA_SAMPLE_S16LE,
    .rate = 48000,
    .channels = 2
  };

  int error;
  /* Probe locally; never leave a freed probe pointer in the stream device. */
  pa_simple *probe = pa_simple_new(NULL, "Moonlight Embedded", PA_STREAM_PLAYBACK,
                                   audio_device, "Streaming", &spec, NULL, NULL, &error);
  if (!probe)
    return false;
  pa_simple_free(probe);
  return true;
}

static int pulse_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  int rc, error;
  unsigned char alsaMapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];

  channelCount = opusConfig->channelCount;
  samplesPerFrame = opusConfig->samplesPerFrame;
  pcmBuffer = malloc(sizeof(short) * channelCount * samplesPerFrame);
  if (pcmBuffer == NULL)
    return -1;

  /* PulseAudio's ALSA channel map uses FL-FR-RL-RR-C-LFE[-SL-SR],
   * independently of CoreELEC's native ALSA mapping override. */
  pulse_alsa_mapping(alsaMapping, opusConfig->mapping, opusConfig->channelCount);

  decoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams, opusConfig->coupledStreams, alsaMapping, &rc);
  if (decoder == NULL || rc != OPUS_OK) {
    fprintf(stderr, "PulseAudio: Opus decoder initialization failed: %d\n", rc);
    pulse_renderer_cleanup();
    return -1;
  }

  pa_sample_spec spec = {
    .format = PA_SAMPLE_S16LE,
    .rate = opusConfig->sampleRate,
    .channels = opusConfig->channelCount
  };

  pa_channel_map map;
  if (!pa_channel_map_init_auto(&map, opusConfig->channelCount, PA_CHANNEL_MAP_ALSA)) {
    fprintf(stderr, "PulseAudio: unsupported channel mapping\n");
    pulse_renderer_cleanup();
    return -1;
  }

  char* audio_device = (char*) context;
  dev = pa_simple_new(NULL, "Moonlight Embedded", PA_STREAM_PLAYBACK, audio_device, "Streaming", &spec, &map, NULL, &error);

  if (!dev) {
    fprintf(stderr, "PulseAudio: %s\n", pa_strerror(error));
    pulse_renderer_cleanup();
    return -1;
  }

  return 0;
}

static void pulse_renderer_decode_and_play_sample(char* data, int length) {
  int decodeLen = opus_multistream_decode(decoder, data, length, pcmBuffer, samplesPerFrame, 0);
  if (decodeLen > 0) {
    int error;
    int rc = pa_simple_write(dev, pcmBuffer, decodeLen * sizeof(short) * channelCount, &error);

    if (rc<0)
      printf("Pulseaudio error: %s\n", pa_strerror(error));
  } else if (decodeLen < 0) {
    printf("Opus error from decode: %d\n", decodeLen);
  }
}

static void pulse_renderer_cleanup() {
  if (decoder != NULL) {
    opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
  }
  if (dev != NULL) {
    pa_simple_free(dev);
    dev = NULL;
  }
  if (pcmBuffer != NULL) {
    free(pcmBuffer);
    pcmBuffer = NULL;
  }
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_pulse = {
  .init = pulse_renderer_init,
  .cleanup = pulse_renderer_cleanup,
  .decodeAndPlaySample = pulse_renderer_decode_and_play_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
