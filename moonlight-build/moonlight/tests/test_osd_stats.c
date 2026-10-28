/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/osd/stats.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  OsdTracker tracker;
  osd_tracker_reset(&tracker);
  AmlFrameStats a = {.submitted=100, .dequeued=95, .uses_v4l2=1, .host_samples=10, .host_tenths_ms=115,
                      .assembly_samples=10, .assembly_us=3300};
  OsdSnapshot s = {0};
  osd_tracker_derive(&tracker, 1000, &a, &s);
  assert(!s.submit_fps && s.host_ms > 1.1f && s.assembly_ms > .32f);
  a.submitted = 160;
  a.dequeued = 154;
  a.host_samples = 20;
  a.host_tenths_ms = 230;
  a.assembly_samples = 20;
  a.assembly_us = 6600;
  s = (OsdSnapshot){0};
  osd_tracker_derive(&tracker, 2000, &a, &s);
  assert(fabs(s.submit_fps - 60.0f) < 0.1f && fabs(s.dequeue_fps - 59.0f) < 0.1f);
  uint32_t v = 0;
  assert(osd_parse_stat_value("buf_avail_num: 5\n", "buf_avail_num", &v) && v==5);
  assert(osd_parse_stat_value("vframe_pool_size=12\n", "vframe_pool_size", &v) && v==12);
  assert(!osd_parse_stat_value("fake_buf_avail_num=3\n", "buf_avail_num", &v));
  assert(!osd_parse_stat_value("buf_avail_num=x\n", "buf_avail_num", &v));
  /* Match the literal output in Pannal's video_sink/video.c. */
  assert(osd_parse_stat_value("vframe_pool_size=12\nvframe buf_avail_num=5\n",
                               "vframe buf_avail_num", &v) && v == 5);
  assert(!osd_parse_stat_value("vframe buf_avail_num=5\n", "buf_avail_num", &v));
  assert(osd_parse_sysfs_counter("42\n", &v) && v == 42);
  assert(osd_parse_sysfs_counter("  123  \n", &v) && v == 123);
  assert(!osd_parse_sysfs_counter("-1", &v));
  assert(!osd_parse_sysfs_counter("12x", &v));
  assert(!osd_parse_sysfs_counter("4294967296", &v));
  assert(!osd_parse_sysfs_counter("", &v));
  OsdTracker sink = {0};
  OsdSnapshot out = {0};
  osd_tracker_sink_update(&sink, 1000, 100, &out);
  assert(!out.has_sink_fps);
  out = (OsdSnapshot){0};
  osd_tracker_sink_update(&sink, 2000, 160, &out);
  assert(out.has_sink_fps && fabs(out.sink_fps - 60.0f) < 0.1f);
  out = (OsdSnapshot){0};
  osd_tracker_sink_update(&sink, 3000, 8, &out);
  assert(!out.has_sink_fps); /* The kernel counter reset. */
  out = (OsdSnapshot){0};
  osd_tracker_sink_update(&sink, 4000, 68, &out);
  assert(out.has_sink_fps && fabs(out.sink_fps - 60.0f) < 0.1f);
  osd_tracker_sink_reset(&sink);
  out = (OsdSnapshot){0};
  osd_tracker_sink_update(&sink, 5000, 98, &out);
  assert(!out.has_sink_fps);
  out = (OsdSnapshot){0};
  osd_tracker_sink_update(&sink, 6000, 158, &out);
  assert(out.has_sink_fps && fabs(out.sink_fps - 60.0f) < 0.1f);
  uint64_t busy=0, total=0;
  assert(osd_parse_cpu_ticks("cpu 10 2 3 80 5 0 0 0 0 0",&busy,&total));
  assert(busy==15 && total==100);
  assert(!osd_parse_cpu_ticks("cpu0 10 2 3 80 5 0 0 0",&busy,&total));
  assert(!osd_parse_cpu_ticks("cpu nope",&busy,&total));
  OsdTracker cpu={0};
  OsdSnapshot cs={0};
  osd_tracker_cpu_update(&cpu,100,1000,&cs);
  assert(!cs.has_sys_cpu);
  cs=(OsdSnapshot){0};
  osd_tracker_cpu_update(&cpu,160,1100,&cs);
  assert(cs.has_sys_cpu && fabs(cs.sys_cpu_percent-60.0f)<0.1f);
  cs=(OsdSnapshot){0};
  osd_tracker_cpu_update(&cpu,2,10,&cs);
  assert(!cs.has_sys_cpu);
  osd_tracker_cpu_reset(&cpu);
  cs=(OsdSnapshot){0};
  osd_tracker_cpu_update(&cpu,30,100,&cs);
  assert(!cs.has_sys_cpu);
  float temp;
  assert(osd_parse_thermal_millidegrees("57000\n",&temp));
  assert(fabs(temp-57.0f)<0.1f);
  assert(!osd_parse_thermal_millidegrees("57",&temp));
  assert(!osd_parse_thermal_millidegrees("-2000",&temp));
  assert(!s.has_submit_mean); /* Sample 1 has no interval history. */
  OsdTracker network_tracker = {0};
  OsdNetworkCounters net = {.video=100,.fec=10,.recovered=2,
                            .failed=0,.oos=0,.invalid=0,.recovery_valid=true};
  OsdSnapshot nw = {0};
  osd_tracker_network_update(&network_tracker,1000,&net,&nw);
  assert(!nw.has_network_rate);
  net.video=160; net.fec=16; net.recovered=4; net.failed=1;
  net.oos=1; net.invalid=1;
  nw=(OsdSnapshot){0};
  osd_tracker_network_update(&network_tracker,2000,&net,&nw);
  assert(nw.has_network_rate && fabsf(nw.video_pps-60.0f)<0.01f &&
         fabsf(nw.fec_pps-6.0f)<0.01f);
  assert(nw.fec_recovered_delta==2 && nw.fec_failed_delta==1 &&
         nw.oos_delta==1 && nw.invalid_delta==1);
  assert(nw.has_network_recovery);
  char net1[512], net2[768];
  osd_format(&nw, net1, sizeof(net1), net2, sizeof(net2));
  assert(strstr(net2,"FECrec +2") && strstr(net2,"FECfail +1"));
  net.recovery_valid=false;
  nw=(OsdSnapshot){0};
  osd_tracker_network_update(&network_tracker,2500,&net,&nw);
  assert(nw.has_network_rate && !nw.has_network_recovery);
  osd_format(&nw, net1, sizeof(net1), net2, sizeof(net2));
  assert(strstr(net2,"FECrec n/a") && strstr(net2,"FECfail n/a") &&
         strstr(net2,"OOS n/a") && strstr(net2,"Invalid n/a"));
  net.video=1; net.fec=0;
  nw=(OsdSnapshot){0};
  osd_tracker_network_update(&network_tracker,3000,&net,&nw);
  assert(!nw.has_network_rate); /* Packet counter reset; not a huge spike. */
  net.video=61; net.fec=10;
  nw=(OsdSnapshot){0};
  osd_tracker_network_update(&network_tracker,4000,&net,&nw);
  assert(nw.has_network_rate && fabsf(nw.video_pps-60.0f)<0.01f);

  OsdTracker submit_tracker = {0};
  AmlFrameStats src = {.submitted=20,.dequeued=18,.uses_v4l2=1,
                        .codec_submit_samples=20,.codec_submit_us=20000,
                        .idr_frames=1,.codec_eagain=2,
                        .backpressure_waits=1,.backpressure_wait_us=5000,
                        .backpressure_drops=0};
  OsdSnapshot dur = {0};
  osd_tracker_derive(&submit_tracker,1000,&src,&dur);
  assert(!dur.has_submit_fps && !dur.has_submit_mean);
  src.submitted=80; src.dequeued=78; src.codec_submit_samples=80;
  src.codec_submit_us=92000; src.idr_frames=3; src.codec_eagain=5;
  src.backpressure_waits=3; src.backpressure_wait_us=20000;
  src.backpressure_drops=1;
  dur=(OsdSnapshot){0};
  osd_tracker_derive(&submit_tracker,2000,&src,&dur);
  assert(dur.has_submit_fps && dur.has_dequeue_fps &&
         dur.has_submit_mean && fabsf(dur.submit_mean_ms-1.2f)<0.001f);
  assert(dur.idr_interval==2 && dur.busy_retries==3);
  assert(dur.backpressure_waits==2 && dur.backpressure_drops==1);
  assert(dur.has_backpressure_wait &&
         fabsf(dur.backpressure_wait_mean_ms-7.5f)<0.001f);
  char line[300], line2[300];
  osd_format(&s, line, sizeof(line), line2, sizeof(line2));
  assert(strstr(line,"Sub 60.0/s") && strstr(line2,"Host 1.15ms"));
  assert(strstr(line2,"SysCPU n/a") && strstr(line2,"Th0 n/a"));
  char bp1[300], bp2[300];
  osd_format(&dur, bp1, sizeof(bp1), bp2, sizeof(bp2));
  assert(strstr(bp2,"EAGAIN +3") && strstr(bp2,"BPWait +2/7.50ms") &&
         strstr(bp2,"BPDrop +1"));
  s.has_sys_cpu=true; s.sys_cpu_percent=55.0f;
  s.has_thermal0=true; s.thermal0_c=57.0f;
  osd_format(&s,line,sizeof(line),line2,sizeof(line2));
  assert(strstr(line2,"SysCPU 55%") && strstr(line2,"Th0 57.0C"));
  assert(strstr(line, "Sink n/a"));
  s.has_sink_fps = true;
  s.sink_fps = 59.8f;
  osd_format(&s, line, sizeof(line), line2, sizeof(line2));
  assert(strstr(line, "Sink 59.8/s"));
  s.v4l2_enabled = false;
  osd_format(&s, line, sizeof(line), line2, sizeof(line2));
  assert(strstr(line, "DQ n/a (direct)"));
  puts("OSD metrics tests passed");
}
