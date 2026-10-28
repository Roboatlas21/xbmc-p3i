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
#include "alsa_resync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus_multistream.h>
#include <alsa/asoundlib.h>

#define CHECK_RETURN(f) if ((rc = f) < 0) { printf("Alsa error code %d\n", rc); return -1; }

static snd_pcm_t *handle;
static OpusMSDecoder* decoder;
static short* pcmBuffer;
static int samplesPerFrame;

typedef struct {
  unsigned int sample_rate;
  unsigned int channels;
  unsigned long period_frames;
  unsigned long buffer_frames;
  unsigned long start_threshold_frames;
  unsigned long avail_min_frames;
  unsigned long long write_calls;
  unsigned long long eagain;
  unsigned long long xruns;
  unsigned long long recover_attempts;
  unsigned long long recoveries;
  unsigned long long short_writes;
  unsigned long long failed_packets;
  unsigned long long decode_errors;
} AlsaRuntimeStats;

static AlsaRuntimeStats alsaStats;

static void alsa_record_write_result(long rc, unsigned long requested) {
  alsaStats.write_calls++;
  if (rc == -EAGAIN)
    alsaStats.eagain++;
  if (rc == -EPIPE)
    alsaStats.xruns++;
  if (rc >= 0 && (unsigned long)rc < requested)
    alsaStats.short_writes++;
}

/* Opt-in, ALSA-only PCM queue recovery. No changes to the default callback. */
static bool alsaResyncEnabled;
static AlsaResyncConfig alsaResyncConfig;
static AlsaResyncStats alsaResyncStats;

static long resync_avail(void *pcm) {
  return snd_pcm_avail_update((snd_pcm_t *)pcm);
}
static long resync_forward(void *pcm, unsigned long frames) {
  return snd_pcm_forward((snd_pcm_t *)pcm, frames);
}
static long resync_write(void *pcm, const void *data, unsigned long frames) {
  return snd_pcm_writei((snd_pcm_t *)pcm, data, frames);
}
static int resync_wait(void *pcm, int milliseconds) {
  return snd_pcm_wait((snd_pcm_t *)pcm, milliseconds);
}
static int resync_recover(void *pcm, int error) {
  return snd_pcm_recover((snd_pcm_t *)pcm, error, 0);
}
static int resync_drop(void *pcm) {
  return snd_pcm_drop((snd_pcm_t *)pcm);
}
static int resync_prepare(void *pcm) {
  return snd_pcm_prepare((snd_pcm_t *)pcm);
}
static const AlsaResyncOps alsaResyncOps = {
  .avail = resync_avail, .forward = resync_forward, .write = resync_write,
  .wait = resync_wait, .recover = resync_recover,
  .drop = resync_drop, .prepare = resync_prepare
};

static int alsa_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  int rc;
  alsaResyncEnabled = false;
  memset(&alsaResyncStats, 0, sizeof(alsaResyncStats));
  memset(&alsaStats, 0, sizeof(alsaStats));
  unsigned char alsaMapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];

  /* Opus uses FL-FR-C-LFE-RL-RR-SL-SR. CoreELEC's audio path expects
   * FL-FR-LFE-C-RL-RR-SL-SR; retain the upstream ALSA mapping elsewhere.
   */
  memcpy(alsaMapping, opusConfig->mapping, sizeof(alsaMapping));
  if (opusConfig->channelCount >= 6) {
#ifdef CORE_ELEC_AUDIO_MAPPING
    alsaMapping[2] = opusConfig->mapping[3];
    alsaMapping[3] = opusConfig->mapping[2];
#else
    alsaMapping[2] = opusConfig->mapping[4];
    alsaMapping[3] = opusConfig->mapping[5];
    alsaMapping[4] = opusConfig->mapping[2];
    alsaMapping[5] = opusConfig->mapping[3];
#endif
  }

  samplesPerFrame = opusConfig->samplesPerFrame;
  pcmBuffer = malloc(sizeof(short) * opusConfig->channelCount * samplesPerFrame);
  if (pcmBuffer == NULL)
    return -1;

  decoder = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams, opusConfig->coupledStreams, alsaMapping, &rc);

  snd_pcm_hw_params_t *hw_params;
  snd_pcm_sw_params_t *sw_params;
  snd_pcm_uframes_t period_size = (opusConfig->sampleRate * 20) / 1000; // 20 ms period
  snd_pcm_uframes_t buffer_size = 3 * period_size; // 60 ms buffer
  unsigned int sampleRate = opusConfig->sampleRate;
  alsaStats.channels = opusConfig->channelCount;

  char* audio_device = (char*) context;
  if (audio_device == NULL)
    audio_device = "sysdefault";

  /* Open PCM device for playback. */
  CHECK_RETURN(snd_pcm_open(&handle, audio_device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK))

  /* Set hardware parameters */
  CHECK_RETURN(snd_pcm_hw_params_malloc(&hw_params));
  CHECK_RETURN(snd_pcm_hw_params_any(handle, hw_params));
  CHECK_RETURN(snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED));
  CHECK_RETURN(snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S16_LE));
  CHECK_RETURN(snd_pcm_hw_params_set_rate_near(handle, hw_params, &sampleRate, NULL));
  CHECK_RETURN(snd_pcm_hw_params_set_channels(handle, hw_params, opusConfig->channelCount));
  CHECK_RETURN(snd_pcm_hw_params_set_period_size_near(handle, hw_params, &period_size, NULL));
  CHECK_RETURN(snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &buffer_size));
  CHECK_RETURN(snd_pcm_hw_params(handle, hw_params));
  /* ALSA may adjust requested sizes; use the actual negotiated geometry. */
  snd_pcm_uframes_t actual_buffer = buffer_size;
  snd_pcm_uframes_t actual_period = period_size;
  if (snd_pcm_hw_params_get_buffer_size(hw_params, &actual_buffer) >= 0)
    buffer_size = actual_buffer;
  if (snd_pcm_hw_params_get_period_size(hw_params, &actual_period, NULL) >= 0)
    period_size = actual_period;
  alsaStats.sample_rate = sampleRate;
  alsaStats.period_frames = (unsigned long)period_size;
  alsaStats.buffer_frames = (unsigned long)buffer_size;
  snd_pcm_hw_params_free(hw_params);

  /* Set software parameters */
  CHECK_RETURN(snd_pcm_sw_params_malloc(&sw_params));
  CHECK_RETURN(snd_pcm_sw_params_current(handle, sw_params));
  CHECK_RETURN(snd_pcm_sw_params_set_avail_min(handle, sw_params, period_size));
  CHECK_RETURN(snd_pcm_sw_params_set_start_threshold(handle, sw_params, period_size));
  CHECK_RETURN(snd_pcm_sw_params(handle, sw_params));

  snd_pcm_uframes_t actual_start_threshold = period_size;
  snd_pcm_uframes_t actual_avail_min = period_size;
  if (snd_pcm_sw_params_get_start_threshold(sw_params, &actual_start_threshold) >= 0)
    alsaStats.start_threshold_frames = (unsigned long)actual_start_threshold;
  else
    alsaStats.start_threshold_frames = (unsigned long)period_size;
  if (snd_pcm_sw_params_get_avail_min(sw_params, &actual_avail_min) >= 0)
    alsaStats.avail_min_frames = (unsigned long)actual_avail_min;
  else
    alsaStats.avail_min_frames = (unsigned long)period_size;
  snd_pcm_sw_params_free(sw_params);

  CHECK_RETURN(snd_pcm_prepare(handle));

  fprintf(stderr,
          "ALSA_CONFIG_V1 rate=%u channels=%u period_frames=%lu buffer_frames=%lu "
          "start_threshold_frames=%lu avail_min_frames=%lu\n",
          alsaStats.sample_rate, alsaStats.channels, alsaStats.period_frames,
          alsaStats.buffer_frames, alsaStats.start_threshold_frames,
          alsaStats.avail_min_frames);

  const char *resync = getenv("MOONLIGHT_ALSA_RESYNC");
  if (resync && strcmp(resync, "1") == 0 &&
      period_size && buffer_size > period_size) {
    alsaResyncConfig = (AlsaResyncConfig){
      .buffer_frames = buffer_size, .period_frames = period_size,
      .channels = opusConfig->channelCount,
      .bytes_per_frame = sizeof(short) * opusConfig->channelCount
    };
    alsaResyncEnabled = true;
    fprintf(stderr, "ALSA resync enabled: %lu buffer frames, %lu period frames, %d channels\n",
            (unsigned long)buffer_size, (unsigned long)period_size,
            opusConfig->channelCount);
  }

  return 0;
}

static void alsa_renderer_cleanup() {
  const unsigned long long write_calls = alsaResyncEnabled ?
      alsaResyncStats.write_calls : alsaStats.write_calls;
  const unsigned long long eagain = alsaResyncEnabled ?
      alsaResyncStats.eagain : alsaStats.eagain;
  const unsigned long long xruns = alsaResyncEnabled ?
      alsaResyncStats.xruns : alsaStats.xruns;
  const unsigned long long recover_attempts = alsaResyncEnabled ?
      alsaResyncStats.recover_attempts : alsaStats.recover_attempts;
  const unsigned long long recoveries = alsaResyncEnabled ?
      alsaResyncStats.recoveries : alsaStats.recoveries;
  const unsigned long long short_writes = alsaResyncEnabled ?
      alsaResyncStats.short_writes : alsaStats.short_writes;
  const unsigned long long failed_packets = alsaResyncEnabled ?
      alsaResyncStats.failed_packets : alsaStats.failed_packets;

  fprintf(stderr,
          "ALSA_STATS_V1 rate=%u channels=%u period_frames=%lu buffer_frames=%lu "
          "start_threshold_frames=%lu avail_min_frames=%lu writes=%llu eagain=%llu "
          "xruns=%llu recover_attempts=%llu recoveries=%llu short_writes=%llu "
          "failed_packets=%llu decode_errors=%llu resync=%d\n",
          alsaStats.sample_rate, alsaStats.channels, alsaStats.period_frames,
          alsaStats.buffer_frames, alsaStats.start_threshold_frames,
          alsaStats.avail_min_frames, write_calls, eagain, xruns,
          recover_attempts, recoveries, short_writes, failed_packets,
          alsaStats.decode_errors, alsaResyncEnabled ? 1 : 0);

  if (alsaResyncEnabled) {
    fprintf(stderr, "ALSA_RESYNC_V1 events=%llu forwarded_frames=%llu resets=%llu "
                    "recoveries=%llu waits=%llu timeouts=%llu dropped_frames=%llu "
                    "failed_packets=%llu\n",
            alsaResyncStats.events, alsaResyncStats.forwarded_frames,
            alsaResyncStats.resets, alsaResyncStats.recoveries,
            alsaResyncStats.waits, alsaResyncStats.wait_timeouts,
            alsaResyncStats.dropped_frames, alsaResyncStats.failed_packets);
    alsaResyncEnabled = false;
  }
  if (decoder != NULL) {
    opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
  }

  if (handle != NULL) {
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    handle = NULL;
  }

  if (pcmBuffer != NULL) {
    free(pcmBuffer);
    pcmBuffer = NULL;
  }
}

static void alsa_renderer_decode_and_play_sample(char* data, int length) {
  int decodeLen = opus_multistream_decode(decoder, data, length, pcmBuffer, samplesPerFrame, 0);
  if (decodeLen > 0) {
    if (alsaResyncEnabled) {
      /* Preserve interleaved frame offsets on short writes and cap waits. */
      (void)alsa_resync_write(&alsaResyncOps, handle, &alsaResyncConfig,
                              &alsaResyncStats, pcmBuffer, (unsigned long)decodeLen);
      return;
    }
    int rc = snd_pcm_writei(handle, pcmBuffer, decodeLen);
    alsa_record_write_result(rc, (unsigned long)decodeLen);
    if (rc < 0) {
      alsaStats.recover_attempts++;
      const int recover_rc = snd_pcm_recover(handle, rc, 0);
      if (recover_rc == 0) {
        alsaStats.recoveries++;
        rc = snd_pcm_writei(handle, pcmBuffer, decodeLen);
        alsa_record_write_result(rc, (unsigned long)decodeLen);
      } else {
        rc = recover_rc;
      }
    }

    if (rc < 0) {
      alsaStats.failed_packets++;
      printf("Alsa error from writei: %d\n", rc);
    } else if (decodeLen != rc) {
      alsaStats.failed_packets++;
      printf("Alsa short write, wrote %d of %d frames\n", rc, decodeLen);
    }
  } else if (decodeLen < 0) {
    alsaStats.decode_errors++;
    printf("Opus error from decode: %d\n", decodeLen);
  }
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_alsa = {
  .init = alsa_renderer_init,
  .cleanup = alsa_renderer_cleanup,
  .decodeAndPlaySample = alsa_renderer_decode_and_play_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
