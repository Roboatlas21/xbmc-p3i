/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/audio/alsa_bounded_write.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
  long replies[20]; int wait_replies[4]; int recover_reply;
  unsigned writes, waits, recoveries, records;
  const unsigned char *pointers[20];
  unsigned long counts[20];
  long recorded[20];
} Mock;
static long write_mock(void *p, const void *data, unsigned long count) {
  Mock *m=p; unsigned i=m->writes++; assert(i<20);
  m->pointers[i]=data; m->counts[i]=count; return m->replies[i];
}
static int wait_mock(void *p,int ms) {
  Mock *m=p; assert(ms==5 && m->waits<4);
  return m->wait_replies[m->waits++];
}
static int recover_mock(void *p,int error) {
  Mock *m=p; assert(error<0); m->recoveries++; return m->recover_reply;
}
static void record_mock(void *p,long rc,unsigned long n) {
  Mock *m=p; assert(m->records<20 && n>0);
  m->recorded[m->records++]=rc;
}
static const AlsaBoundedOps ops={
  .write=write_mock,.wait=wait_mock,.recover=recover_mock,.record=record_mock
};
static Mock fresh(void) {
  Mock m={0}; for(unsigned i=0;i<20;i++)m.replies[i]=240;
  for(unsigned i=0;i<4;i++)m.wait_replies[i]=1;
  return m;
}
int main(void) {
  unsigned char audio[2880]={0}; /* 240 frames, 6 channels */
  Mock m=fresh();
  AlsaBoundedResult r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(!r.remaining && m.writes==1 && !m.waits && !m.recoveries);
  m=fresh(); m.replies[0]=100; m.replies[1]=140;
  r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(!r.remaining && m.writes==2 && m.counts[1]==140);
  assert(m.pointers[1]==audio+1200 && m.records==2);
  m=fresh(); m.replies[0]=-EAGAIN; m.replies[1]=240;
  r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(!r.remaining && m.waits==1 && m.recoveries==0 && m.writes==2);
  m=fresh(); m.replies[0]=-EPIPE; m.replies[1]=240;
  r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(!r.remaining && m.recoveries==1 && m.writes==2);
  m=fresh(); m.replies[0]=0; m.wait_replies[0]=0;
  r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(r.remaining==240 && m.writes==1 && m.waits==1 && !m.recoveries);
  m=fresh(); m.replies[0]=100; m.replies[1]=-EAGAIN; m.wait_replies[0]=0;
  r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(r.remaining==140 && m.pointers[1]==audio+1200 && m.counts[1]==140);
  m=fresh(); m.replies[0]=-EPIPE; m.recover_reply=-EIO;
  r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(r.remaining==240 && m.recoveries==1);
  /* Repeated EAGAIN must stop after two bounded waits, not recover it. */
  m=fresh(); for(unsigned i=0;i<20;i++)m.replies[i]=-EAGAIN;
  r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(r.remaining==240 && m.waits==2 && m.writes==3);
  assert(r.recovery_attempts==0 && !r.successful_recoveries);

  /* Sixteen partial writes must terminate without replaying consumed frames. */
  m=fresh(); for(unsigned i=0;i<20;i++)m.replies[i]=1;
  r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(r.remaining==224 && m.writes==16 && m.pointers[15]==audio+15*12);
  assert(m.counts[15]==225);

  /* Failed recovery is counted as attempted, never successful. */
  m=fresh(); m.replies[0]=-EPIPE; m.recover_reply=-EIO;
  r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(r.remaining==240 && r.recovery_attempts==1);
  assert(r.successful_recoveries==0 && m.writes==1);

  m=fresh(); m.replies[0]=-EPIPE;
  r=alsa_bounded_write(&ops,&m,audio,240,12);
  assert(r.recovery_attempts==1 && r.successful_recoveries==1);
  puts("ALSA bounded normal writer mock tests passed");
  return 0;
}
