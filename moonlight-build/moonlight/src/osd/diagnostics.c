/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diagnostics.h"
#include <inttypes.h>
#include <stdio.h>

void stream_diagnostics_format(StreamDiagnosticTracker *t, uint64_t now_ms,
                                bool native_available, const AmlFrameStats *v,
                                const OsdSnapshot *s, const AudioDiagnostics *a,
                                char *out, size_t size) {
  char first[512], second[768], native[320] = "native=n/a", audio[512] = "audio=n/a";
  osd_format(s, first, sizeof(first), second, sizeof(second));
  if (!t->initialized) t->start_ms = now_ms;
#define VD(f) ((uint32_t)(v->f - t->video.f))
#define AD(f) ((uint32_t)(a->f - t->audio.f))
  if (native_available && t->initialized) {
    char queue[32] = "n/a", age[32] = "n/a";
    if (VD(queue_wait_samples))
      snprintf(queue, sizeof(queue), "%.2f", VD(queue_wait_us) / (double)VD(queue_wait_samples) / 1000.0);
    if (v->input_frames)
      snprintf(age, sizeof(age), "%u", (uint32_t)now_ms - v->last_input_ms);
    snprintf(native, sizeof(native),
             "native=aml cb=+%u cb_age_ms=%s cb_gap50=+%u cb_gap100=+%u "
             "submit20=+%u submit50=+%u queue_mean_ms=%s queue20=+%u",
             VD(input_frames), age, VD(input_gaps_50ms), VD(input_gaps_100ms),
             VD(submit_slow_20ms), VD(submit_slow_50ms), queue, VD(queue_wait_20ms));
  } else if (native_available) snprintf(native, sizeof(native), "native=aml baseline=1");
  if (a->active && t->initialized && t->audio.active) {
    char delay[32] = "n/a", age[32] = "n/a", cb_age[32] = "n/a";
    if (a->delay_valid && a->sample_rate)
      snprintf(delay, sizeof(delay), "%.2f", a->delay_frames * 1000.0 / a->sample_rate);
    if (a->delay_sample_ms)
      snprintf(age, sizeof(age), "%u", (uint32_t)now_ms - a->delay_sample_ms);
    if (a->callbacks)
      snprintf(cb_age, sizeof(cb_age), "%u", (uint32_t)now_ms - a->last_callback_ms);
    snprintf(audio, sizeof(audio),
             "audio=alsa cb=+%u cb_age_ms=%s gap50=+%u slow20=+%u slow50=+%u "
             "delay_ms=%s delay_age_ms=%s state=%u writes=+%u eagain=+%u xruns=+%u "
             "recover_attempts=+%u recoveries=+%u short=+%u failed=+%u opus_errors=+%u resync=%u",
             AD(callbacks), cb_age, AD(callback_gaps_50ms), AD(slow_20ms), AD(slow_50ms),
             delay, age, a->pcm_state, AD(writes), AD(eagain), AD(xruns),
             AD(recover_attempts), AD(recoveries), AD(short_writes), AD(failed_packets),
             AD(decode_errors), a->resync);
  } else if (a->active) snprintf(audio, sizeof(audio), "audio=alsa baseline=1");
  snprintf(out, size, "STREAM_DIAG_V1 t_ms=%" PRIu64 " dt_ms=%" PRIu64
           " | %s | %s | %s | %s",
           now_ms - t->start_ms, t->initialized ? now_ms - t->last_ms : 0,
           first, second, native, audio);
  t->video = *v;
  t->audio = *a;
  t->last_ms = now_ms;
  t->initialized = true;
#undef VD
#undef AD
}
