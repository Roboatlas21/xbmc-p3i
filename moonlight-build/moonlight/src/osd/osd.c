/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "osd.h"
#include "stats.h"
#ifdef HAVE_DRM_OSD
#include "drm.h"
#endif
#include <Limelight.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t ticks_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int bounded_int(const char *str, int low, int high, int *out) {
  if (!str || !*str) return -1;
  char *end;
  errno = 0;
  long value = strtol(str, &end, 10);
  if (errno || end == str || *end || value < low || value > high) return -1;
  *out = (int)value;
  return 0;
}

bool osd_parse_options(const char *input, OsdOptions *out) {
  if (!out) return false;
  *out = (OsdOptions){.x = -1, .y = 40, .font_size = 20, .interval_s = 1.5, .log = false};
  if (!input || !*input) return true;
  if (strlen(input) >= 256) return false;
  char buf[256];
  strcpy(buf, input);
  char *save = NULL;
  for (char *p = strtok_r(buf, " \t", &save); p; p = strtok_r(NULL, " \t", &save)) {
    if (strcmp(p, "-log") == 0) { out->log = true; continue; }
    if (strcmp(p, "-font-size") && strcmp(p, "-y") && strcmp(p, "-x") && strcmp(p, "-interval")) return false;
    char *value = strtok_r(NULL, " \t", &save);
    if (!value) return false;
    if (!strcmp(p, "-interval")) {
      char *end;
      errno = 0;
      const double v = strtod(value, &end);
      if (errno || end == value || *end || !(v >= 0.1 && v <= 60.0)) return false;
      out->interval_s = v;
    } else if (!strcmp(p, "-font-size")) {
      if (bounded_int(value, 8, 100, &out->font_size)) return false;
    } else if (!strcmp(p, "-x")) {
      if (bounded_int(value, 0, 10000, &out->x)) return false;
    } else if (!strcmp(p, "-y")) {
      if (bounded_int(value, 0, 10000, &out->y)) return false;
    }
  }
  return true;
}

static void kernel_queue(OsdSnapshot *out) {
  FILE *f = fopen("/sys/class/video/vframe_states", "r");
  if (!f) return;
  char buffer[2048] = {0};
  const size_t n = fread(buffer, 1, sizeof(buffer)-1, f);
  fclose(f);
  if (!n) return;
  /* NG's video.c prints "vframe buf_avail_num", not "buf_avail_num". */
  out->has_kernel_queue = osd_parse_stat_value(buffer, "vframe buf_avail_num", &out->kernel_queue);
  out->has_kernel_pool = osd_parse_stat_value(buffer, "vframe_pool_size", &out->kernel_pool);
}

/* System-wide video-sink selected-frame counter. Read-only, no VFM/DRM
 * ownership and no writes to global kernel parameters. */
static void sink_frames(OsdTracker *tracker, uint64_t now_ms, OsdSnapshot *out) {
  FILE *f = fopen("/sys/module/amvideo/parameters/display_frame_count", "r");
  if (!f) {
    osd_tracker_sink_reset(tracker);
    return;
  }
  char buf[64] = {0};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  uint32_t count = 0;
  if (!n || !osd_parse_sysfs_counter(buf, &count)) {
    osd_tracker_sink_reset(tracker);
    return;
  }
  osd_tracker_sink_update(tracker, now_ms, count, out);
}

/* Read-only whole-device CPU and thermal_zone0 metrics, sampled at the OSD
 * refresh cadence. Missing/unknown readings remain n/a. */
static void system_stats(OsdTracker *tracker, OsdSnapshot *out) {
  FILE *f=fopen("/proc/stat","r");
  if (f) {
    char line[512]={0};
    uint64_t busy=0,total=0;
    if (fgets(line,sizeof(line),f) &&
        osd_parse_cpu_ticks(line,&busy,&total))
      osd_tracker_cpu_update(tracker,busy,total,out);
    else osd_tracker_cpu_reset(tracker);
    fclose(f);
  } else osd_tracker_cpu_reset(tracker);
  f=fopen("/sys/class/thermal/thermal_zone0/temp","r");
  if (f) {
    char value[64]={0};
    float celsius;
    if (fgets(value,sizeof(value),f) &&
        osd_parse_thermal_millidegrees(value,&celsius)) {
      out->thermal0_c=celsius;
      out->has_thermal0=true;
    }
    fclose(f);
  }
}

/* Match one decoder channel to this Moonlight stream's codec and dimensions.
 * This is read-only, and handles the case of no decoder, duplicate channels,
 * reset counters, and kernels without vdec_status. */
static void decoder_stats(OsdTracker *tracker, uint64_t now_ms,
                          const AmlFrameStats *native, OsdSnapshot *out) {
  if (!native->stream_width || !native->stream_height ||
      (native->codec_family != 1 && native->codec_family != 2)) {
    osd_tracker_decoder_reset(tracker);
    return;
  }
  FILE *f = fopen("/sys/class/vdec/vdec_status", "r");
  if (!f) {
    osd_tracker_decoder_reset(tracker);
    return;
  }
  char buf[8192] = {0};
  const size_t read_bytes = fread(buf, 1, sizeof(buf) - 1, f);
  const bool eof = feof(f) != 0;
  fclose(f);
  if (!read_bytes || (!eof && read_bytes == sizeof(buf) - 1)) {
    osd_tracker_decoder_reset(tracker);
    return;
  }
  OsdVdecCounters decoded = {0};
  if (!osd_vdec_parse(buf, native->stream_width, native->stream_height,
                      native->codec_family, &decoded)) {
    osd_tracker_decoder_reset(tracker);
    return;
  }
  osd_tracker_decoder_update(tracker, now_ms, &decoded, out);
}

static void sample(OsdTracker *tracker, OsdSnapshot *out) {
  memset(out, 0, sizeof(*out));
  /* The library is loaded RTLD_GLOBAL by platform_check() on Amlogic. */
  AmlFrameStats native = {0};
  typedef void (*NativeStatsFn)(AmlFrameStats *);
  NativeStatsFn fetch = (NativeStatsFn)dlsym(RTLD_DEFAULT, "aml_osd_snapshot");
  if (fetch) fetch(&native);
  const uint64_t now_ms = ticks_ms();
  osd_tracker_derive(tracker, now_ms, &native, out);
  sink_frames(tracker, now_ms, out);
  decoder_stats(tracker, now_ms, &native, out);
  system_stats(tracker, out);
  const int queued = LiGetPendingVideoFrames();
  if (queued >= 0) {
    out->pending_frames = (uint32_t)queued;
    out->has_pending_frames = true;
  }
  uint32_t rtt, variance;
  if (LiGetEstimatedRttInfo(&rtt, &variance)) {
    out->has_rtt = true;
    out->rtt_ms = rtt;
  }
  const RTP_VIDEO_STATS *network = LiGetRTPVideoStats();
  if (network) {
    out->packet_video = network->packetCountVideo;
    out->packet_fec = network->packetCountFec;
    /* FEC recovery failures, NOT every missing video packet. */
    out->packet_lost = network->packetCountFecFailed;
    const OsdNetworkCounters counters = {
      .video = network->packetCountVideo,
      .fec = network->packetCountFec,
      .recovered = network->packetCountFecRecovered,
      .failed = network->packetCountFecFailed,
      .oos = network->packetCountOOS,
      .invalid = network->packetCountInvalid
    };
    osd_tracker_network_update(tracker, now_ms, &counters, out);
  }
  kernel_queue(out);
}

static void *osd_thread(void *arg) {
  Osd *osd = arg;
  OsdTracker tracker;
  osd_tracker_reset(&tracker);
#ifdef HAVE_DRM_OSD
  OsdDrm *renderer = osd_drm_open(&osd->options);
#else
  void *renderer = NULL;
#endif
  if (!renderer) fprintf(stderr, "OSD: DRM overlay unavailable; use -osdargs '-log' for a text log\n");
  while (!__atomic_load_n(&osd->stopping, __ATOMIC_RELAXED)) {
    OsdSnapshot result;
    sample(&tracker, &result);
    char a[512], b[512];
    osd_format(&result, a, sizeof(a), b, sizeof(b));
    if (osd->options.log) fprintf(stderr, "OSD: %s | %s\n", a, b);
#ifdef HAVE_DRM_OSD
    if (renderer && !osd_drm_draw(renderer, a, b)) {
      fprintf(stderr, "OSD: DRM atomic update failed, disabling visual overlay\n");
      osd_drm_close(renderer);
      renderer = NULL;
    }
#endif
    unsigned wait_ms = (unsigned)(osd->options.interval_s * 1000);
    while (wait_ms && !__atomic_load_n(&osd->stopping, __ATOMIC_RELAXED)) {
      unsigned step = wait_ms < 100 ? wait_ms : 100;
      struct timespec period = {0, (long)step * 1000000L};
      nanosleep(&period, NULL);
      wait_ms -= step;
    }
  }
#ifdef HAVE_DRM_OSD
  if (renderer) osd_drm_close(renderer);
#endif
  return NULL;
}

bool osd_start(Osd *osd, const char *args) {
  memset(osd, 0, sizeof(*osd));
  if (!osd_parse_options(args, &osd->options)) {
    fprintf(stderr, "OSD: invalid -osdargs; native statistics overlay disabled\n");
    return false;
  }
  osd->started = pthread_create(&osd->thread, NULL, osd_thread, osd) == 0;
  if (!osd->started) fprintf(stderr, "OSD: could not start overlay thread\n");
  return osd->started;
}

void osd_stop(Osd *osd) {
  if (!osd->started) return;
  __atomic_store_n(&osd->stopping, 1, __ATOMIC_RELAXED);
  pthread_join(osd->thread, NULL);
  osd->started = false;
}
