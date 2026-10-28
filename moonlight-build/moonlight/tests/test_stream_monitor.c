/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../src/osd/osd.h"
#include "../src/osd/drm.h"
#include "../src/osd/stats.h"
#include "../src/audio/diagnostics.h"
#include <Limelight.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned drm_opens;
OsdDrm *osd_drm_open(const OsdOptions *o) { (void)o; ++drm_opens; return NULL; }
bool osd_drm_draw(OsdDrm *d, const char *a, const char *b) {
  (void)d; (void)a; (void)b; assert(0); return false;
}
void osd_drm_close(OsdDrm *d) { (void)d; assert(0); }
void aml_osd_snapshot(AmlFrameStats *v) { *v=(AmlFrameStats){.input_frames=1}; }
void alsa_diagnostics_snapshot(AudioDiagnostics *a) { *a=(AudioDiagnostics){.active=1}; }
int LiGetPendingVideoFrames(void) { return 0; }
bool LiGetEstimatedRttInfo(uint32_t *rtt, uint32_t *variance) {
  *rtt=1; *variance=0; return true;
}
const RTP_VIDEO_STATS *LiGetRTPVideoStats(void) {
  static RTP_VIDEO_STATS s; return &s;
}
static uint64_t now_ms(void) {
  struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
  return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
int main(void) {
  Osd osd;
  unsetenv("MOONLIGHT_STREAM_DIAGNOSTICS");
  assert(!osd_start_monitor(&osd,NULL,false,true) && !osd.started);
  setenv("MOONLIGHT_STREAM_DIAGNOSTICS","0",1);
  assert(!osd_diagnostics_enabled());
  setenv("MOONLIGHT_STREAM_DIAGNOSTICS","1",1);
  FILE *log=tmpfile(); assert(log);
  int saved=dup(STDERR_FILENO); assert(saved>=0);
  assert(dup2(fileno(log),STDERR_FILENO)>=0);
  assert(osd_start_monitor(&osd,"-this-is-irrelevant-without-overlay",false,true));
  const uint64_t started=now_ms();
  while (now_ms()-started<1250) {
    struct timespec delay={0,10000000}; nanosleep(&delay,NULL);
  }
  const uint64_t stopping=now_ms();
  osd_stop(&osd);
  assert(now_ms()-stopping<1000 && !osd.started);
  osd_stop(&osd);
  fflush(stderr);
  assert(dup2(saved,STDERR_FILENO)>=0); close(saved);
  rewind(log);
  char line[2500]; unsigned samples=0, headers=0;
  while (fgets(line,sizeof(line),log)) {
    if (strstr(line,"STREAM_DIAG_V1")) ++samples;
    if (strstr(line,"STREAM_DIAG_START_V1")) ++headers;
    assert(!strstr(line,"DRM overlay unavailable"));
  }
  fclose(log);
  assert(drm_opens==0 && headers==1 && samples==2);
  /* A fast visual refresh must not consume diagnostic deltas or flood the log. */
  log=tmpfile(); assert(log);
  saved=dup(STDERR_FILENO); assert(saved>=0);
  assert(dup2(fileno(log),STDERR_FILENO)>=0);
  assert(osd_start_monitor(&osd,"-interval 0.1 -log",true,true));
  const uint64_t again=now_ms();
  while (now_ms()-again<1250) {
    struct timespec delay={0,10000000}; nanosleep(&delay,NULL);
  }
  osd_stop(&osd);
  fflush(stderr); assert(dup2(saved,STDERR_FILENO)>=0); close(saved);
  rewind(log); unsigned visual=0; samples=0;
  while (fgets(line,sizeof(line),log)) {
    if (strstr(line,"STREAM_DIAG_V1")) ++samples;
    if (!strncmp(line,"OSD: Dec",8)) ++visual;
  }
  fclose(log);
  assert(drm_opens==1 && samples==2 && visual>=5);
  puts("Stream monitor: headless idle heartbeat, no DRM access, prompt shutdown and independent OSD cadence passed");
}
