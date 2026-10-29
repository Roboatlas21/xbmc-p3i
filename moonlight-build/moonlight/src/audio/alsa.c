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
#include "alsa_bounded_write.h"
#include "alsa_suspend_recovery.h"
#include "diagnostics.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <opus_multistream.h>
#include <alsa/asoundlib.h>

#define CHECK_RETURN(f) if ((rc = f) < 0) { printf("Alsa error code %d\n", rc); goto error; }

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
  unsigned long long dropped_frames, zero_progress_callbacks;
} AlsaRuntimeStats;

static AlsaRuntimeStats alsaStats;
static AudioDiagnostics audioDiagnostics;
static bool audioDiagnosticsEnabled;

static uint64_t audio_ticks_us(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void alsa_diagnostics_snapshot(AudioDiagnostics *out) {
  if (!out) return;
#define LOAD(f) out->f = __atomic_load_n(&audioDiagnostics.f, __ATOMIC_RELAXED)
  LOAD(active); LOAD(callbacks); LOAD(last_callback_ms);
  LOAD(slow_20ms); LOAD(slow_50ms); LOAD(callback_gaps_50ms);
  LOAD(writes); LOAD(eagain); LOAD(xruns); LOAD(recover_attempts);
  LOAD(recoveries); LOAD(short_writes); LOAD(failed_packets); LOAD(decode_errors);
  LOAD(dropped_frames); LOAD(zero_progress_callbacks);
  LOAD(sample_rate); LOAD(delay_valid); LOAD(delay_sample_ms);
  LOAD(pcm_state); LOAD(resync); LOAD(delay_frames);
#undef LOAD
}

static void alsa_record_write_result(long rc, unsigned long requested) {
  if (!audioDiagnosticsEnabled) return;
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
static int recover_resume_once(void *pcm) {
  return snd_pcm_resume((snd_pcm_t *)pcm);
}
static int recover_prepare(void *pcm) {
  return snd_pcm_prepare((snd_pcm_t *)pcm);
}
static int recover_standard(void *pcm, int error) {
  return snd_pcm_recover((snd_pcm_t *)pcm, error, 0);
}
static const AlsaRecoveryOps bounded_recovery_ops = {
  .resume = recover_resume_once,
  .prepare = recover_prepare,
  .recover = recover_standard
};
static int resync_recover(void *pcm, int error) {
  return alsa_recover_bounded(&bounded_recovery_ops, pcm, error);
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

static long bounded_write(void *ctx, const void *data, unsigned long frames) {
  return snd_pcm_writei((snd_pcm_t *)ctx, data, frames);
}
static int bounded_wait(void *ctx, int ms) {
  return snd_pcm_wait((snd_pcm_t *)ctx, ms);
}
static int bounded_recover(void *ctx, int error) {
  if (audioDiagnosticsEnabled) alsaStats.recover_attempts++;
  int rc = alsa_recover_bounded(&bounded_recovery_ops, ctx, error);
  if (rc == 0 && audioDiagnosticsEnabled) alsaStats.recoveries++;
  return rc;
}
static void bounded_record(void *ctx, long rc, unsigned long frames) {
  (void)ctx;
  alsa_record_write_result(rc, frames);
}
static const AlsaBoundedOps bounded_ops_diagnostics = {
  .write = bounded_write, .wait = bounded_wait,
  .recover = bounded_recover, .record = bounded_record
};
static const AlsaBoundedOps bounded_ops_normal = {
  .write = bounded_write, .wait = bounded_wait,
  .recover = bounded_recover, .record = NULL
};
static const AlsaBoundedOps *active_bounded_ops = &bounded_ops_normal;

static void audio_diagnostics_record(uint64_t start_us) {
  if (!audioDiagnosticsEnabled) return;
  const uint64_t now_us = audio_ticks_us();
  const uint32_t now_ms = (uint32_t)(now_us / 1000);
  if (now_us >= start_us) {
    if (now_us - start_us > 20000)
      __atomic_fetch_add(&audioDiagnostics.slow_20ms, 1, __ATOMIC_RELAXED);
    if (now_us - start_us > 50000)
      __atomic_fetch_add(&audioDiagnostics.slow_50ms, 1, __ATOMIC_RELAXED);
  }
  /* Run read-only PCM queries on the audio thread, at most once per second.
   * The sampler only reads published scalars, including when this thread stalls. */
  const uint32_t last = __atomic_load_n(&audioDiagnostics.delay_sample_ms, __ATOMIC_RELAXED);
  if (!last || now_ms - last >= 1000) {
    snd_pcm_sframes_t delay = 0;
    const int rc = snd_pcm_delay(handle, &delay);
    __atomic_store_n(&audioDiagnostics.delay_frames, (int32_t)delay, __ATOMIC_RELAXED);
    __atomic_store_n(&audioDiagnostics.delay_valid, rc >= 0, __ATOMIC_RELAXED);
    __atomic_store_n(&audioDiagnostics.pcm_state, (uint32_t)snd_pcm_state(handle), __ATOMIC_RELAXED);
    __atomic_store_n(&audioDiagnostics.delay_sample_ms, now_ms, __ATOMIC_RELAXED);
  }
#define PUBLISH(f, normal, resync) __atomic_store_n(&audioDiagnostics.f, \
    (uint32_t)(alsaResyncEnabled ? alsaResyncStats.resync : alsaStats.normal), __ATOMIC_RELAXED)
  PUBLISH(writes, write_calls, write_calls);
  PUBLISH(eagain, eagain, eagain);
  PUBLISH(xruns, xruns, xruns);
  PUBLISH(recover_attempts, recover_attempts, recover_attempts);
  PUBLISH(recoveries, recoveries, recoveries);
  PUBLISH(short_writes, short_writes, short_writes);
  PUBLISH(failed_packets, failed_packets, failed_packets);
#undef PUBLISH
  __atomic_store_n(&audioDiagnostics.decode_errors, (uint32_t)alsaStats.decode_errors, __ATOMIC_RELAXED);
  __atomic_store_n(&audioDiagnostics.dropped_frames, (uint32_t)(alsaResyncEnabled ? alsaResyncStats.dropped_frames : alsaStats.dropped_frames), __ATOMIC_RELAXED);
  __atomic_store_n(&audioDiagnostics.zero_progress_callbacks, (uint32_t)alsaStats.zero_progress_callbacks, __ATOMIC_RELAXED);
}

static int alsa_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  int rc;
  snd_pcm_hw_params_t *hw_params = NULL;
  snd_pcm_sw_params_t *sw_params = NULL;
  alsaResyncEnabled = false;
  memset(&alsaResyncStats, 0, sizeof(alsaResyncStats));
  memset(&alsaStats, 0, sizeof(alsaStats));
  memset(&audioDiagnostics, 0, sizeof(audioDiagnostics));
  const char *diagnostics = getenv("MOONLIGHT_STREAM_DIAGNOSTICS");
  audioDiagnosticsEnabled = diagnostics && strcmp(diagnostics, "1") == 0;
  active_bounded_ops = audioDiagnosticsEnabled ?
      &bounded_ops_diagnostics : &bounded_ops_normal;
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
  if (decoder == NULL || rc != OPUS_OK) {
    fprintf(stderr, "Opus decoder initialization failed: %d\n", rc);
    goto error;
  }
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
  hw_params = NULL;

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
  sw_params = NULL;

  CHECK_RETURN(snd_pcm_prepare(handle));

  if (audioDiagnosticsEnabled)
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

  __atomic_store_n(&audioDiagnostics.sample_rate, sampleRate, __ATOMIC_RELAXED);
  __atomic_store_n(&audioDiagnostics.resync, alsaResyncEnabled, __ATOMIC_RELAXED);
  __atomic_store_n(&audioDiagnostics.active, audioDiagnosticsEnabled, __ATOMIC_RELAXED);
  return 0;
error:
  /* Common does not call cleanup() when init() fails. Release partial state
   * here, including parameter blocks, and allow a later init to start fresh. */
  if (hw_params)
    snd_pcm_hw_params_free(hw_params);
  if (sw_params)
    snd_pcm_sw_params_free(sw_params);
  if (handle) {
    snd_pcm_close(handle);
    handle = NULL;
  }
  if (decoder) {
    opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
  }
  free(pcmBuffer);
  pcmBuffer = NULL;
  return -1;
}

static void alsa_renderer_cleanup() {
  if (audioDiagnosticsEnabled) {
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
          "failed_packets=%llu decode_errors=%llu dropped_frames=%llu zero_progress_callbacks=%llu resync=%d\n",
          alsaStats.sample_rate, alsaStats.channels, alsaStats.period_frames,
          alsaStats.buffer_frames, alsaStats.start_threshold_frames,
          alsaStats.avail_min_frames, write_calls, eagain, xruns,
          recover_attempts, recoveries, short_writes, failed_packets,
          alsaStats.decode_errors,
          alsaResyncEnabled ? alsaResyncStats.dropped_frames : alsaStats.dropped_frames,
          alsaStats.zero_progress_callbacks, alsaResyncEnabled ? 1 : 0);

  if (alsaResyncEnabled) {
    fprintf(stderr, "ALSA_RESYNC_V1 events=%llu forwarded_frames=%llu resets=%llu "
                    "recoveries=%llu waits=%llu timeouts=%llu dropped_frames=%llu "
                    "failed_packets=%llu\n",
            alsaResyncStats.events, alsaResyncStats.forwarded_frames,
            alsaResyncStats.resets, alsaResyncStats.recoveries,
            alsaResyncStats.waits, alsaResyncStats.wait_timeouts,
            alsaResyncStats.dropped_frames, alsaResyncStats.failed_packets);
  }
  } /* diagnostics summary */
  alsaResyncEnabled = false;
  if (decoder != NULL) {
    opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
  }

  if (handle != NULL) {
    /* Streaming teardown must not wait for queued PCM to finish playing. */
    snd_pcm_drop(handle);
    snd_pcm_close(handle);
    handle = NULL;
  }

  if (pcmBuffer != NULL) {
    free(pcmBuffer);
    pcmBuffer = NULL;
  }
}

static void alsa_renderer_decode_and_play_sample(char* data, int length) {
  uint64_t start_us = 0;
  if (audioDiagnosticsEnabled) {
    start_us = audio_ticks_us();
    const uint32_t ms = (uint32_t)(start_us / 1000);
    const uint32_t prior = __atomic_load_n(&audioDiagnostics.last_callback_ms, __ATOMIC_RELAXED);
    if (__atomic_load_n(&audioDiagnostics.callbacks, __ATOMIC_RELAXED) && ms - prior > 50)
      __atomic_fetch_add(&audioDiagnostics.callback_gaps_50ms, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&audioDiagnostics.last_callback_ms, ms, __ATOMIC_RELAXED);
    __atomic_fetch_add(&audioDiagnostics.callbacks, 1, __ATOMIC_RELAXED);
  }
  int decodeLen = opus_multistream_decode(decoder, (const unsigned char *)data,
                                         length, pcmBuffer, samplesPerFrame, 0);
  if (decodeLen > 0) {
    if (alsaResyncEnabled) {
      /* Preserve interleaved frame offsets on short writes and cap waits. */
      (void)alsa_resync_write(&alsaResyncOps, handle, &alsaResyncConfig,
                              &alsaResyncStats, pcmBuffer, (unsigned long)decodeLen);
      audio_diagnostics_record(start_us);
      return;
    }
    AlsaBoundedResult result = alsa_bounded_write(
        active_bounded_ops, handle, pcmBuffer, (unsigned long)decodeLen,
        sizeof(short) * alsaStats.channels);
    if (result.remaining) {
      alsaStats.failed_packets++;
      if (audioDiagnosticsEnabled) {
        alsaStats.dropped_frames += result.remaining;
        if (result.remaining == (unsigned long)decodeLen) alsaStats.zero_progress_callbacks++;
      }
      fprintf(stderr, "ALSA: incomplete PCM write (%lu/%d frames remaining, last=%d)\n",
              result.remaining, decodeLen, result.last_error);
    }
  } else if (decodeLen < 0) {
    alsaStats.decode_errors++;
    printf("Opus error from decode: %d\n", decodeLen);
  }
  audio_diagnostics_record(start_us);
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_alsa = {
  .init = alsa_renderer_init,
  .cleanup = alsa_renderer_cleanup,
  .decodeAndPlaySample = alsa_renderer_decode_and_play_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
