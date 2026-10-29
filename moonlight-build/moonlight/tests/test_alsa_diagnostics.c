/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <alsa/asoundlib.h>
#include <opus_multistream.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

static uint64_t fake_us=1000000;
static unsigned delay_calls;
static int write_error, delay_error;
static int fake_clock(clockid_t id, struct timespec *ts) {
  (void)id; ts->tv_sec=fake_us/1000000; ts->tv_nsec=(fake_us%1000000)*1000; return 0;
}
static int fake_decode(OpusMSDecoder *d, const unsigned char *data, opus_int32 n,
                       opus_int16 *out, int count, int fec) {
  (void)d; (void)data; (void)n; (void)out; (void)count; (void)fec; return 240;
}
static snd_pcm_sframes_t fake_write(snd_pcm_t *p, const void *data, snd_pcm_uframes_t n) {
  (void)p; (void)data; fake_us+=25000;
  int rc=write_error; write_error=0; return rc ? rc : (snd_pcm_sframes_t)n;
}
static int fake_recover(snd_pcm_t *p, int err, int silent) {
  (void)p; (void)silent; return err==-EPIPE ? 0 : err;
}
static int fake_delay(snd_pcm_t *p, snd_pcm_sframes_t *frames) {
  (void)p; ++delay_calls; *frames=960; return delay_error;
}
static snd_pcm_state_t fake_state(snd_pcm_t *p) { (void)p; return SND_PCM_STATE_RUNNING; }
static snd_pcm_sframes_t fake_avail(snd_pcm_t *p) { (void)p; return 2880; }
static snd_pcm_sframes_t fake_forward(snd_pcm_t *p, snd_pcm_uframes_t n) {
  (void)p; (void)n; assert(0); return 0;
}
static int fake_wait(snd_pcm_t *p, int ms) { (void)p; (void)ms; assert(0); return 0; }
static int fake_drop(snd_pcm_t *p) { (void)p; assert(0); return 0; }
static int fake_prepare(snd_pcm_t *p) { (void)p; assert(0); return 0; }
#define clock_gettime fake_clock
#define opus_multistream_decode fake_decode
#define snd_pcm_writei fake_write
#define snd_pcm_recover fake_recover
#define snd_pcm_delay fake_delay
#define snd_pcm_state fake_state
#define snd_pcm_avail_update fake_avail
#define snd_pcm_forward fake_forward
#define snd_pcm_wait fake_wait
#define snd_pcm_drop fake_drop
#define snd_pcm_prepare fake_prepare
#include "../src/audio/alsa.c"

int main(void) {
  short buffer[480];
  pcmBuffer=buffer; handle=(snd_pcm_t *)(uintptr_t)1; decoder=(OpusMSDecoder *)(uintptr_t)1;
  samplesPerFrame=240; audioDiagnosticsEnabled=true;
  alsaStats.channels=2;
  active_bounded_ops=&bounded_ops_diagnostics;
  audioDiagnostics.active=1; audioDiagnostics.sample_rate=48000;
  alsa_renderer_decode_and_play_sample("x",1);
  AudioDiagnostics a;
  alsa_diagnostics_snapshot(&a);
  assert(a.callbacks==1 && a.slow_20ms==1 && a.slow_50ms==0 && a.writes==1);
  assert(a.delay_valid && a.delay_frames==960 && delay_calls==1);
  fake_us+=100000;
  write_error=-EPIPE;
  alsa_renderer_decode_and_play_sample("x",1);
  alsa_diagnostics_snapshot(&a);
  assert(a.callbacks==2 && a.callback_gaps_50ms==1 && a.xruns==1);
  assert(a.recover_attempts==1 && a.recoveries==1 && a.failed_packets==0);
  assert(delay_calls==1 && a.slow_20ms==2); /* At most one queue query/second. */
  fake_us+=1000000; write_error=-EAGAIN; delay_error=-EPIPE;
  alsa_renderer_decode_and_play_sample("x",1);
  alsa_diagnostics_snapshot(&a);
  assert(!a.delay_valid && delay_calls==2 && a.eagain==1 && a.failed_packets==1);
  assert(a.recover_attempts==2 && a.recoveries==1);
  /* The opt-in resync branch publishes its own write/error counters too. */
  alsaResyncEnabled=true;
  alsaResyncConfig=(AlsaResyncConfig){.buffer_frames=2880,.period_frames=960,
                                     .channels=2,.bytes_per_frame=4};
  alsa_renderer_decode_and_play_sample("x",1);
  alsa_diagnostics_snapshot(&a);
  assert(a.writes==alsaResyncStats.write_calls && a.writes==1 && a.eagain==0);
  audioDiagnosticsEnabled=false;
  active_bounded_ops=&bounded_ops_normal;
  unsigned calls=a.callbacks;
  alsa_renderer_decode_and_play_sample("x",1);
  alsa_diagnostics_snapshot(&a);
  assert(a.callbacks==calls && delay_calls==2);
  puts("Actual ALSA callbacks: queue sampling, XRUN recovery, EAGAIN, resync and disabled diagnostics passed");
}
