/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/audio/alsa_resync.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  long avail;
  long forward_return; /* LONG_MIN: return requested frame count. */
  unsigned long last_forward;
  unsigned int forward_calls, drop_calls, prepare_calls;
  int drop_rc, prepare_rc, recover_rc;
  int wait_returns[8];
  unsigned int wait_calls, recover_calls, write_calls;
  int last_wait_ms, last_recover;
  long write_returns[20];
  const unsigned char *seen_ptrs[20];
  unsigned long seen_counts[20];
} Mock;
static long mock_avail(void *ctx) { return ((Mock *)ctx)->avail; }
static long mock_forward(void *ctx, unsigned long frames) {
  Mock *m = ctx;
  m->forward_calls++;
  m->last_forward = frames;
  return m->forward_return == LONG_MIN ? (long)frames : m->forward_return;
}
static long mock_write(void *ctx, const void *data, unsigned long frames) {
  Mock *m = ctx;
  unsigned i = m->write_calls++;
  assert(i < 20);
  m->seen_ptrs[i] = data;
  m->seen_counts[i] = frames;
  return m->write_returns[i] == LONG_MIN ? (long)frames : m->write_returns[i];
}
static int mock_wait(void *ctx, int ms) {
  Mock *m = ctx;
  m->last_wait_ms = ms;
  unsigned i = m->wait_calls++;
  assert(i < 8);
  return m->wait_returns[i];
}
static int mock_recover(void *ctx, int error) {
  Mock *m = ctx;
  m->recover_calls++;
  m->last_recover = error;
  return m->recover_rc;
}
static int mock_drop(void *ctx) {
  Mock *m = ctx;
  m->drop_calls++;
  return m->drop_rc;
}
static int mock_prepare(void *ctx) {
  Mock *m = ctx;
  m->prepare_calls++;
  return m->prepare_rc;
}
static const AlsaResyncOps ops = {
  .avail=mock_avail, .forward=mock_forward, .write=mock_write,
  .wait=mock_wait, .recover=mock_recover,
  .drop=mock_drop, .prepare=mock_prepare
};
static Mock new_mock(unsigned long buffer) {
  Mock m = {.avail=(long)buffer, .forward_return=LONG_MIN};
  for (unsigned i = 0; i < 20; i++) m.write_returns[i] = LONG_MIN;
  for (unsigned i = 0; i < 8; i++) m.wait_returns[i] = 1;
  return m;
}
static const AlsaResyncConfig stereo = {
  .buffer_frames=2880, .period_frames=960, .channels=2, .bytes_per_frame=4
};
static unsigned char pcm_data[4096];
#define PLAY(m,c,s,frames) alsa_resync_write(&ops, &(m), &(c), &(s), pcm_data, (frames))

int main(void) {
  AlsaResyncStats s={0};
  Mock m=new_mock(stereo.buffer_frames);
  assert(PLAY(m,stereo,s,240)==240);
  assert(m.write_calls==1 && m.forward_calls==0 && !s.events);
  assert(s.write_calls==1 && !s.eagain && !s.xruns && !s.short_writes);

  /* Ordinary fast path has no waits or pointer modification. */
  m = new_mock(stereo.buffer_frames);
  m.avail=stereo.buffer_frames + 720;
  s=(AlsaResyncStats){0};
  assert(PLAY(m,stereo,s,240)==240);
  assert(m.forward_calls==1 && m.last_forward==1680);
  assert(s.events==1 && s.forwarded_frames==1680 && !s.resets);

  AlsaResyncConfig surround=stereo;
  surround.channels=6; surround.bytes_per_frame=12;
  m=new_mock(surround.buffer_frames); m.avail=3600;
  assert(alsa_resync_write(&ops,&m,&surround,&s,pcm_data,240)==240);
  assert(m.last_forward==2640);

  /* If ALSA can't advance its cursor, drop+prepare instead of retrying
   * indefinitely or playing stale queued samples. */
  m=new_mock(stereo.buffer_frames); m.avail=3600; m.forward_return=-ENOSYS;
  s=(AlsaResyncStats){0};
  assert(PLAY(m,stereo,s,240)==240);
  assert(m.forward_calls==1 && m.drop_calls==1 && m.prepare_calls==1);
  assert(s.resets==1 && s.events==1);

  m=new_mock(stereo.buffer_frames); m.avail=3600; m.forward_return=100;
  assert(PLAY(m,stereo,s,240)==240);
  assert(m.drop_calls==1); /* Partial forward needs a reset too. */

  m=new_mock(stereo.buffer_frames); m.avail=20000;
  assert(PLAY(m,stereo,s,240)==240);
  assert(m.forward_calls==0 && m.drop_calls==1);

  m=new_mock(stereo.buffer_frames); m.avail=3600; m.forward_return=-ENOSYS;
  m.drop_rc=-EIO; s=(AlsaResyncStats){0};
  assert(PLAY(m,stereo,s,240)==-EIO);
  assert(m.write_calls==0 && s.failed_packets==1 && s.dropped_frames==240);

  /* Short writes must advance the interleaved pointer, not replay it. */
  m=new_mock(stereo.buffer_frames); s=(AlsaResyncStats){0};
  m.write_returns[0]=100; m.write_returns[1]=140;
  assert(PLAY(m,stereo,s,240)==240);
  assert(m.write_calls==2 && m.seen_counts[1]==140);
  assert(m.seen_ptrs[1]==pcm_data+100*stereo.bytes_per_frame);
  assert(s.write_calls==2 && s.short_writes==1);

  m=new_mock(stereo.buffer_frames); s=(AlsaResyncStats){0};
  m.write_returns[0]=-EAGAIN;
  assert(PLAY(m,stereo,s,240)==240);
  assert(m.wait_calls==1 && m.last_wait_ms==60 && m.write_calls==2);
  assert(s.waits==1 && s.dropped_frames==0);
  assert(s.eagain==1 && s.write_calls==2);

  m=new_mock(stereo.buffer_frames); s=(AlsaResyncStats){0};
  m.write_returns[0]=-EAGAIN; m.wait_returns[0]=0;
  assert(PLAY(m,stereo,s,240)==0);
  assert(m.write_calls==1 && s.wait_timeouts==1 && s.dropped_frames==240);

  m=new_mock(stereo.buffer_frames); s=(AlsaResyncStats){0};
  m.write_returns[0]=-EPIPE;
  assert(PLAY(m,stereo,s,240)==240);
  assert(m.recover_calls==1 && m.last_recover==-EPIPE);
  assert(s.recover_attempts==1 && s.recoveries==1 && s.xruns==1);
  assert(s.write_calls==2);

  m=new_mock(stereo.buffer_frames); s=(AlsaResyncStats){0};
  m.avail=-EPIPE;
  assert(PLAY(m,stereo,s,240)==240);
  assert(m.recover_calls==1 && s.recover_attempts==1 && s.recoveries==1);
  assert(s.xruns==1);

  m=new_mock(stereo.buffer_frames); s=(AlsaResyncStats){0};
  m.write_returns[0]=-EPIPE; m.recover_rc=-ENODEV;
  assert(PLAY(m,stereo,s,240)==0);
  assert(s.failed_packets==1 && s.dropped_frames==240 && m.write_calls==1);
  assert(s.recover_attempts==1 && s.recoveries==0 && s.xruns==1);

  m=new_mock(stereo.buffer_frames); s=(AlsaResyncStats){0};
  m.write_returns[0]=0; m.wait_returns[0]=-EPIPE;
  assert(PLAY(m,stereo,s,240)==240);
  assert(m.last_recover==-EPIPE && m.write_calls==2);
  assert(s.recover_attempts==1 && s.recoveries==1 && s.xruns==1);

  /* Even a malfunctioning backend cannot make us spin forever. */
  m=new_mock(stereo.buffer_frames); s=(AlsaResyncStats){0};
  for(unsigned i=0;i<20;i++)m.write_returns[i]=1;
  assert(PLAY(m,stereo,s,240)==16);
  assert(m.write_calls==16 && s.dropped_frames==224);

  m=new_mock(stereo.buffer_frames); s=(AlsaResyncStats){0};
  AlsaResyncConfig invalid=stereo; invalid.period_frames=invalid.buffer_frames;
  assert(alsa_resync_write(&ops,&m,&invalid,&s,pcm_data,240)==-EINVAL);
  assert(m.write_calls==0);
  assert(alsa_resync_write(&ops,&m,&stereo,&s,NULL,240)==-EINVAL);
  assert(alsa_resync_write(&ops,&m,&stereo,&s,pcm_data,0)==0);
  puts("ALSA queue resynchronization tests passed");
  return 0;
}
