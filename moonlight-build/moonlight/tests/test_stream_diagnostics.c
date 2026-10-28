/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/osd/diagnostics.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  StreamDiagnosticTracker tracker = {0};
  AmlFrameStats v = {.input_frames=100, .last_input_ms=1000};
  AudioDiagnostics a = {.active=1, .callbacks=200, .sample_rate=48000};
  OsdSnapshot s = {0};
  char line[2304];
  stream_diagnostics_format(&tracker, 1000, true, &v, &s, &a, line, sizeof(line));
  assert(strstr(line,"t_ms=0 dt_ms=0"));
  assert(strstr(line,"native=aml baseline=1") && strstr(line,"audio=alsa baseline=1"));
  v.input_frames += 50; v.last_input_ms=1950;
  v.input_gaps_50ms=3; v.input_gaps_100ms=1;
  v.submit_slow_20ms=4; v.submit_slow_50ms=2;
  v.queue_wait_samples=2; v.queue_wait_us=60000; v.queue_wait_20ms=1;
  a.callbacks+=190; a.last_callback_ms=1990;
  a.slow_20ms=3; a.slow_50ms=1; a.xruns=2; a.recoveries=2;
  a.callback_gaps_50ms=1; a.delay_valid=1; a.delay_frames=960;
  a.delay_sample_ms=1900;
  stream_diagnostics_format(&tracker,2000,true,&v,&s,&a,line,sizeof(line));
  assert(strstr(line,"t_ms=1000 dt_ms=1000"));
  assert(strstr(line,"cb=+50 cb_age_ms=50 cb_gap50=+3 cb_gap100=+1"));
  assert(strstr(line,"submit20=+4 submit50=+2 queue_mean_ms=30.00 queue20=+1"));
  assert(strstr(line,"audio=alsa cb=+190 cb_age_ms=10 gap50=+1 slow20=+3 slow50=+1"));
  assert(strstr(line,"delay_ms=20.00 delay_age_ms=100") && strstr(line,"xruns=+2"));
  /* No new callbacks: still log, exposing increasing age rather than hiding a stall. */
  stream_diagnostics_format(&tracker,3000,true,&v,&s,&a,line,sizeof(line));
  assert(strstr(line,"cb=+0 cb_age_ms=1050") && strstr(line,"audio=alsa cb=+0 cb_age_ms=1010"));
  assert(strstr(line,"queue_mean_ms=n/a") && strstr(line,"xruns=+0"));
  assert(strstr(line,"delay_age_ms=1100"));
  /* u32 counters and clock stamps wrap on ARM32 without fabricating huge deltas. */
  tracker.audio.writes=UINT32_MAX-1;
  a.writes=2; a.last_callback_ms=UINT32_MAX-5; a.delay_valid=0;
  v.last_input_ms=UINT32_MAX-3;
  stream_diagnostics_format(&tracker,(uint64_t)UINT32_MAX+10,true,&v,&s,&a,line,sizeof(line));
  assert(strstr(line,"writes=+4") && strstr(line,"delay_ms=n/a"));
  assert(strstr(line,"cb_age_ms=13") && strstr(line,"audio=alsa cb=+0 cb_age_ms=15"));
  /* VFM/non-ALSA measurements must explicitly remain unavailable. */
  a=(AudioDiagnostics){0};
  stream_diagnostics_format(&tracker,(uint64_t)UINT32_MAX+1010,false,&v,&s,&a,line,sizeof(line));
  assert(strstr(line,"native=n/a") && strstr(line,"audio=n/a"));
  char small[12];
  memset(small, 'x', sizeof(small));
  stream_diagnostics_format(&tracker,(uint64_t)UINT32_MAX+2010,false,&v,&s,&a,small,8);
  assert(small[7]=='\0' && small[8]=='x');
  puts("Stream diagnostic intervals, idle stalls, unavailable data and u32 wrap passed");
}
