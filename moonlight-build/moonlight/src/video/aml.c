/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 * Copyright (C) 2016 OtherCrashOverride, Daniel Mehrwald
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

#include <Limelight.h>

#include <sys/utsname.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <codec.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>

#include <linux/videodev2.h>

#include "../util.h"
#include "video.h"
#include "aml_profile.h"
#include "aml_tuning.h"
#include "aml_low_latency.h"
#include "aml_pacing.h"
#include "../osd/stats.h"

#define SYNC_OUTSIDE 0x02
#define UCODE_IP_ONLY_PARAM 0x08
#define MAX_WRITE_ATTEMPTS 5
#define EAGAIN_SLEEP_TIME 2 * 1000

/* Match Skupi's pre-write decoder back-pressure shape: start throttling
 * before the Amlogic input buffer is effectively full, poll in coarse
 * intervals, and never block the Moonlight submit thread indefinitely. */
#define AML_VBUF_HIGH_WATER_PERCENT 95U
#define AML_VBUF_POLL_INTERVAL_US (5U * 1000U)
#define AML_VBUF_WAIT_TIMEOUT_US (30U * 1000U)
#define AML_VBUF_MAX_POLLS (AML_VBUF_WAIT_TIMEOUT_US / AML_VBUF_POLL_INTERVAL_US)
#define AML_VDEC_ERROR_IDR_COOLDOWN_MS 1000U

#define AML_PACING_QUEUE_CAPACITY 16U
#define AML_PACING_MAX_WAITING 3U
#define AML_PACING_POLL_TIMEOUT_MS 50

/* Same control values used by Skupi and CoreELEC Kodi's AML decoder. */
#define AML_TRICKMODE_NONE 0U
#define AML_AV_SYNC_THRESH (90000U * 30U)
#define AML_VIDEO_DELAY_LIMIT_MS 1000

/*
 * Vortex's CoreELEC NO binary exposes MOONLIGHT_HEVC_RFI=0 to disable
 * reference-frame invalidation when a decoder cannot reliably handle it.
 * Leave upstream behaviour unchanged unless the caller explicitly opts out.
 */
extern DECODER_RENDERER_CALLBACKS decoder_callbacks_aml;

static void __attribute__((constructor)) aml_configure_hevc_rfi(void) {
  const char *rfi = getenv("MOONLIGHT_HEVC_RFI");
  if (rfi != NULL && strcmp(rfi, "0") == 0) {
    decoder_callbacks_aml.capabilities &=
        ~CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC;
    fprintf(stderr, "HEVC reference-frame invalidation disabled (MOONLIGHT_HEVC_RFI=0)\n");
  }
}

static codec_para_t codecParam = { 0 };
static pthread_t displayThread;
static bool display_thread_started = false;
static int videoFd = -1;
static bool done = false;
static AmlProfile stream_profile;
static AmlTuning stream_tuning;
static AmlLowLatency stream_low_latency;
static bool balanced_pacing;
static uint32_t pacing_units_written;
static uint32_t pacing_frames_returned;
static uint32_t pacing_frames_dropped;

/* Cheap opt-in 32-bit atomics; no lock or per-frame print in the video path.
 * These are submission/V4L2-dequeue events, not guaranteed hardware presents. */
static AmlFrameStats osd_stats;
static bool osd_stats_enabled;
static uint32_t last_dequeue_ms;
static uint32_t last_vdec_error_count;
static uint64_t last_vdec_error_idr_ms;

static bool aml_vbuf_needs_wait(const struct buf_status *vbuf, size_t incoming) {
  if (!vbuf || vbuf->size <= 0 || vbuf->data_len < 0 || vbuf->free_len < 0)
    return false;

  const uint64_t size = (uint64_t)(unsigned int)vbuf->size;
  const uint64_t data = (uint64_t)(unsigned int)vbuf->data_len;
  const uint64_t free_bytes = (uint64_t)(unsigned int)vbuf->free_len;
  const uint64_t high_water = size * AML_VBUF_HIGH_WATER_PERCENT / 100U;

  return data + (uint64_t)incoming > high_water ||
         free_bytes < (uint64_t)incoming;
}

/* Fail open if this older libamcodec cannot report buffer state reliably.
 * codec_write()/EAGAIN remains the compatibility fallback in that case. */
static bool aml_wait_for_vbuf_room(size_t incoming, unsigned int *polls) {
  if (polls) *polls = 0;

  for (unsigned int i = 0;; i++) {
    struct buf_status vbuf = { 0 };
    if (codec_get_vbuf_state(&codecParam, &vbuf) != 0)
      return true;

    if (!aml_vbuf_needs_wait(&vbuf, incoming))
      return true;

    if (i >= AML_VBUF_MAX_POLLS)
      return false;

    if (polls) (*polls)++;
    usleep(AML_VBUF_POLL_INTERVAL_US);
  }
}

/* Match Skupi's post-submit decoder recovery. libamcodec exposes an
 * accumulated decoder error_count; a newly increased count means the
 * current reference chain may no longer be trustworthy. Request an IDR
 * through DR_NEED_IDR, but no more than once per second. */
static bool aml_vdec_error_needs_idr(void) {
  struct vdec_status vdec = { 0 };
  if (codec_get_vdec_state(&codecParam, &vdec) != 0)
    return false;

  if (vdec.error_count < last_vdec_error_count) {
    /* Decoder reset/counter restart: establish a new baseline. */
    last_vdec_error_count = vdec.error_count;
    return false;
  }

  if (vdec.error_count == last_vdec_error_count)
    return false;

  last_vdec_error_count = vdec.error_count;

  const uint64_t now_ms = LiGetMillis();
  if (last_vdec_error_idr_ms != 0 &&
      now_ms - last_vdec_error_idr_ms < AML_VDEC_ERROR_IDR_COOLDOWN_MS)
    return false;

  last_vdec_error_idr_ms = now_ms;
  fprintf(stderr, "AML: decoder error count %u, asking for a keyframe\n",
          vdec.error_count);
  return true;
}

void aml_osd_snapshot(AmlFrameStats *out) {
  if (!out) return;
  out->submitted = __atomic_load_n(&osd_stats.submitted, __ATOMIC_RELAXED);
  out->dequeued = __atomic_load_n(&osd_stats.dequeued, __ATOMIC_RELAXED);
  out->uses_v4l2 = __atomic_load_n(&osd_stats.uses_v4l2, __ATOMIC_RELAXED);
  out->submit_failures = __atomic_load_n(&osd_stats.submit_failures, __ATOMIC_RELAXED);
  out->codec_eagain = __atomic_load_n(&osd_stats.codec_eagain, __ATOMIC_RELAXED);
  out->backpressure_waits = __atomic_load_n(&osd_stats.backpressure_waits, __ATOMIC_RELAXED);
  out->backpressure_wait_us = __atomic_load_n(&osd_stats.backpressure_wait_us, __ATOMIC_RELAXED);
  out->backpressure_drops = __atomic_load_n(&osd_stats.backpressure_drops, __ATOMIC_RELAXED);
  out->idr_frames = __atomic_load_n(&osd_stats.idr_frames, __ATOMIC_RELAXED);
  out->codec_submit_us = __atomic_load_n(&osd_stats.codec_submit_us, __ATOMIC_RELAXED);
  out->codec_submit_samples = __atomic_load_n(&osd_stats.codec_submit_samples, __ATOMIC_RELAXED);
  out->dequeue_gaps_25ms = __atomic_load_n(&osd_stats.dequeue_gaps_25ms, __ATOMIC_RELAXED);
  out->dequeue_gaps_33ms = __atomic_load_n(&osd_stats.dequeue_gaps_33ms, __ATOMIC_RELAXED);
  out->host_samples = __atomic_load_n(&osd_stats.host_samples, __ATOMIC_RELAXED);
  out->host_tenths_ms = __atomic_load_n(&osd_stats.host_tenths_ms, __ATOMIC_RELAXED);
  out->assembly_samples = __atomic_load_n(&osd_stats.assembly_samples, __ATOMIC_RELAXED);
  out->assembly_us = __atomic_load_n(&osd_stats.assembly_us, __ATOMIC_RELAXED);
  out->stream_width = __atomic_load_n(&osd_stats.stream_width, __ATOMIC_RELAXED);
  out->stream_height = __atomic_load_n(&osd_stats.stream_height, __ATOMIC_RELAXED);
  out->codec_family = __atomic_load_n(&osd_stats.codec_family, __ATOMIC_RELAXED);
  out->qbuf_failures = __atomic_load_n(&osd_stats.qbuf_failures, __ATOMIC_RELAXED);
#define COPY_DIAG(field) out->field = __atomic_load_n(&osd_stats.field, __ATOMIC_RELAXED)
  COPY_DIAG(input_frames);
  COPY_DIAG(last_input_ms);
  COPY_DIAG(input_gaps_50ms);
  COPY_DIAG(input_gaps_100ms);
  COPY_DIAG(submit_slow_20ms);
  COPY_DIAG(submit_slow_50ms);
  COPY_DIAG(queue_wait_us);
  COPY_DIAG(queue_wait_samples);
  COPY_DIAG(queue_wait_20ms);
#undef COPY_DIAG
}

void *pkt_buf = NULL;
size_t pkt_buf_size = 0;

static void aml_record_dequeue(void) {
  aml_profile_display_poll(&stream_profile, true);
  if (!osd_stats_enabled)
    return;

  __atomic_fetch_add(&osd_stats.dequeued, 1, __ATOMIC_RELAXED);
  const uint32_t ms = (uint32_t)(aml_profile_now_us() / 1000);
  if (last_dequeue_ms) {
    const uint32_t gap = ms - last_dequeue_ms;
    if (gap > 25)
      __atomic_fetch_add(&osd_stats.dequeue_gaps_25ms, 1, __ATOMIC_RELAXED);
    if (gap > 33)
      __atomic_fetch_add(&osd_stats.dequeue_gaps_33ms, 1, __ATOMIC_RELAXED);
  }
  last_dequeue_ms = ms;
}

static int aml_qbuf_index(uint32_t index, bool drop) {
  struct v4l2_buffer vbuf = { 0 };
  vbuf.index = index;
  vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (drop)
    vbuf.flags = V4L2_BUF_FLAG_DONE;

  if (ioctl(videoFd, VIDIOC_QBUF, &vbuf) == 0)
    return 0;

  const int error = errno;
  const uint32_t failures = __atomic_add_fetch(&osd_stats.qbuf_failures, 1, __ATOMIC_RELAXED);
  if (failures == 1 || failures % 60 == 0) {
    fprintf(stderr, "AML: VIDIOC_QBUF%s failed (%u times): %s\n",
            drop ? " (drop)" : "", failures, strerror(error));
  }
  return -1;
}

/* Skupi balanced pacing:
 * - collect decoded amlvideo buffers into a small local ring;
 * - keep at most three waiting by returning older buffers with DONE set;
 * - poll the Amlogic control fd and return one normal buffer per event.
 *
 * The decoder/RFI/HEVC work remains underneath this policy. Routing is
 * selected only through codecParam.video_path; no global VFM rewrite is used. */
void* aml_display_thread(void* unused) {
  uint32_t queue[AML_PACING_QUEUE_CAPACITY] = { 0 };
  unsigned int head = 0;
  unsigned int count = 0;
  unsigned int dq_errors = 0;

  printf("AML: pump gives back one frame per refresh, at most %u waiting\n",
         AML_PACING_MAX_WAITING);

  while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
    while (count < AML_PACING_QUEUE_CAPACITY) {
      struct v4l2_buffer vbuf = { 0 };
      vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

      if (ioctl(videoFd, VIDIOC_DQBUF, &vbuf) < 0) {
        if (errno == EAGAIN) {
          aml_profile_display_poll(&stream_profile, false);
        } else {
          dq_errors++;
          if (dq_errors == 1 || dq_errors % 60 == 0) {
            fprintf(stderr, "AML: VIDIOC_DQBUF failed (%u times): %s\n",
                    dq_errors, strerror(errno));
          }
        }
        break;
      }

      dq_errors = 0;
      queue[(head + count) % AML_PACING_QUEUE_CAPACITY] = vbuf.index;
      count++;
      aml_record_dequeue();
    }

    while (count > AML_PACING_MAX_WAITING) {
      const int qbuf_result = aml_qbuf_index(queue[head], true);
      if (!aml_pacing_complete_qbuf(qbuf_result, AML_PACING_QUEUE_CAPACITY,
                                    &head, &count))
        break;
      __atomic_fetch_add(&pacing_frames_dropped, 1, __ATOMIC_RELAXED);
    }

    struct pollfd pfd = {
      .fd = codecParam.cntl_handle,
      .events = POLLOUT,
      .revents = 0
    };
    const int poll_result = poll(&pfd, 1, AML_PACING_POLL_TIMEOUT_MS);
    if (poll_result > 0 && (pfd.revents & POLLOUT) && count > 0) {
      const int qbuf_result = aml_qbuf_index(queue[head], false);
      if (aml_pacing_complete_qbuf(qbuf_result, AML_PACING_QUEUE_CAPACITY,
                                   &head, &count))
        __atomic_fetch_add(&pacing_frames_returned, 1, __ATOMIC_RELAXED);
    }
  }

  while (count > 0) {
    const int qbuf_result = aml_qbuf_index(queue[head], false);
    if (!aml_pacing_complete_qbuf(qbuf_result, AML_PACING_QUEUE_CAPACITY,
                                  &head, &count))
      break;
  }

  printf("Display thread terminated\n");
  return NULL;
}

int aml_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  // Diagnostics are off unless explicitly enabled. No per-frame logging.
  const char *pacing = getenv("MOONLIGHT_FRAME_PACING");
  balanced_pacing = pacing != NULL && strcmp(pacing, "balanced") == 0;
  if (pacing != NULL && strcmp(pacing, "latency") != 0 &&
      strcmp(pacing, "balanced") != 0) {
    fprintf(stderr, "AML: unknown frame pacing '%s'; using latency\n", pacing);
    balanced_pacing = false;
  }
  aml_profile_init(&stream_profile, getenv("MOONLIGHT_AML_PROFILE"));
  osd_stats_enabled = getenv("MOONLIGHT_AML_OSD") &&
                      strcmp(getenv("MOONLIGHT_AML_OSD"), "1") == 0;
  memset(&osd_stats, 0, sizeof(osd_stats));
  if (osd_stats_enabled) {
    __atomic_store_n(&osd_stats.stream_width, (uint32_t)width, __ATOMIC_RELAXED);
    __atomic_store_n(&osd_stats.stream_height, (uint32_t)height, __ATOMIC_RELAXED);
    const uint32_t family = (videoFormat & VIDEO_FORMAT_MASK_H264) ? 1 :
                            (videoFormat & VIDEO_FORMAT_MASK_H265) ? 2 : 0;
    __atomic_store_n(&osd_stats.codec_family, family, __ATOMIC_RELAXED);
  }
  last_dequeue_ms = 0;
  last_vdec_error_count = 0;
  last_vdec_error_idr_ms = 0;
  __atomic_store_n(&pacing_units_written, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&pacing_frames_returned, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&pacing_frames_dropped, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&done, false, __ATOMIC_RELEASE);
  display_thread_started = false;

  if (balanced_pacing) {
    videoFd = open("/dev/video10", O_RDONLY | O_NONBLOCK);
    if (videoFd < 0) {
      fprintf(stderr, "AML: can't open /dev/video10: %s\n", strerror(errno));
      return -3;
    }
  } else {
    videoFd = -1;
  }

  codecParam.handle             = -1;
  codecParam.cntl_handle        = -1;
  codecParam.audio_utils_handle = -1;
  codecParam.sub_handle         = -1;
  codecParam.has_video          = 1;
  codecParam.noblock            = 0;
  codecParam.stream_type        = STREAM_TYPE_ES_VIDEO;
  codecParam.am_sysinfo.param   = 0;

#ifdef STREAM_TYPE_FRAME
  codecParam.dec_mode           = STREAM_TYPE_FRAME;
#endif

  /* Skupi uses the tunnel path for lowest latency and Kodi's amlvideo
   * path for balanced pacing. These enum values are part of the NG
   * libamcodec/kernel ABI. */
  codecParam.video_path = balanced_pacing ?
      FRAME_BASE_PATH_AMLVIDEO_AMVIDEO : FRAME_BASE_PATH_TUNNEL_MODE;

  if (videoFormat & VIDEO_FORMAT_MASK_H264) {
    if (width > 1920 || height > 1080) {
      codecParam.video_type = VFORMAT_H264_4K2K;
      codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_H264_4K2K;
    } else {
      codecParam.video_type = VFORMAT_H264;
      codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_H264;

      // Workaround for decoding special case of C1, 1080p, H264
      int major, minor;
      struct utsname name;
      uname(&name);
      int ret = sscanf(name.release, "%d.%d", &major, &minor);
      if (ret == 2 && !(major > 3 || (major == 3 && minor >= 14)) && width == 1920 && height == 1080)
          codecParam.am_sysinfo.param = (void*) UCODE_IP_ONLY_PARAM;
    }
  } else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
    codecParam.video_type = VFORMAT_HEVC;
    codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_HEVC;
#ifdef CODEC_TAG_AV1
  } else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
    codecParam.video_type = VFORMAT_AV1;
    codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_AV1;
#endif
  } else {
    printf("Video format not supported\n");
    return -1;
  }

  codecParam.am_sysinfo.width = width;
  codecParam.am_sysinfo.height = height;
  codecParam.am_sysinfo.rate = 96000 / redrawRate;
  codecParam.am_sysinfo.param = (void*) ((size_t) codecParam.am_sysinfo.param | SYNC_OUTSIDE);

  aml_low_latency_init(&stream_low_latency, NULL);
  (void)aml_low_latency_enable(&stream_low_latency);

  aml_tuning_init(&stream_tuning, NULL);
  aml_tuning_configure(&stream_tuning,
      (videoFormat & VIDEO_FORMAT_MASK_H265) != 0,
      getenv("MOONLIGHT_AML_TUNING_PROBE"));

  int ret;
  if ((ret = codec_init(&codecParam)) != 0) {
    fprintf(stderr, "codec_init error: %x\n", ret);
    if (videoFd >= 0) {
      close(videoFd);
      videoFd = -1;
    }
    aml_tuning_restore(&stream_tuning);
    aml_low_latency_restore(&stream_low_latency);
    return -2;
  }

  printf("AML: frame pacing %s\n",
         balanced_pacing ? "balanced" : "latency");

  /* Match Skupi/CoreELEC's AML control initialization. Global tsync is owned
   * by Luna's CoreELEC session wrapper, not this native decoder. These calls
   * establish normal playback mode,
   * bound the driver's video-delay accounting, and use the same AV/sync
   * thresholds as Kodi before freerun presentation is enabled below. */
  (void)codec_pause(&codecParam);
  (void)codec_set_cntl_mode(&codecParam, AML_TRICKMODE_NONE);
  (void)codec_set_video_delay_limited_ms(&codecParam,
                                         AML_VIDEO_DELAY_LIMIT_MS);
  (void)codec_set_cntl_avthresh(&codecParam, AML_AV_SYNC_THRESH);
  (void)codec_set_cntl_syncthresh(&codecParam, 0);

  if ((ret = codec_set_freerun_mode(&codecParam, 1)) != 0) {
    fprintf(stderr, "Can't set Freerun mode: %x\n", ret);
    codec_close(&codecParam);
    if (videoFd >= 0) {
      close(videoFd);
      videoFd = -1;
    }
    aml_tuning_restore(&stream_tuning);
    aml_low_latency_restore(&stream_low_latency);
    return -2;
  }

  if (balanced_pacing) {
    if (codecParam.cntl_handle < 0) {
      fprintf(stderr, "AML: balanced pacing requires a video control handle\n");
      codec_close(&codecParam);
      close(videoFd);
      videoFd = -1;
      aml_tuning_restore(&stream_tuning);
      aml_low_latency_restore(&stream_low_latency);
      return -3;
    }

    if (pthread_create(&displayThread, NULL, aml_display_thread, NULL) == 0) {
      display_thread_started = true;
      __atomic_store_n(&osd_stats.uses_v4l2, 1, __ATOMIC_RELAXED);
    } else {
      fprintf(stderr, "AML: can't start the amlvideo pump\n");
      codec_close(&codecParam);
      close(videoFd);
      videoFd = -1;
      aml_tuning_restore(&stream_tuning);
      aml_low_latency_restore(&stream_low_latency);
      return -3;
    }
  }

  ensure_buf_size(&pkt_buf, &pkt_buf_size, INITIAL_DECODER_BUFFER_SIZE);

  return 0;
}

void aml_cleanup() {
  if (videoFd >= 0) {
    __atomic_store_n(&done, true, __ATOMIC_RELEASE);
    if (display_thread_started) {
      pthread_join(displayThread, NULL);
      display_thread_started = false;
    }
    close(videoFd);
    videoFd = -1;
  }

  codec_close(&codecParam);
  aml_tuning_restore(&stream_tuning);
  aml_low_latency_restore(&stream_low_latency);
  aml_profile_report(&stream_profile, stderr);
  if (balanced_pacing) {
    fprintf(stderr,
            "AML: %u units written, %u frames given back one per refresh, "
            "%u dropped by the pump, %u QBUF failures\n",
            __atomic_load_n(&pacing_units_written, __ATOMIC_RELAXED),
            __atomic_load_n(&pacing_frames_returned, __ATOMIC_RELAXED),
            __atomic_load_n(&pacing_frames_dropped, __ATOMIC_RELAXED),
            __atomic_load_n(&osd_stats.qbuf_failures, __ATOMIC_RELAXED));
  }
  free(pkt_buf);
  pkt_buf = NULL;
  pkt_buf_size = 0;
}

int aml_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  const uint64_t submit_start_us =
      (stream_profile.enabled || osd_stats_enabled) ? aml_profile_now_us() : 0;
  unsigned int write_calls = 0;
  unsigned int eagain_count = 0;
  unsigned int backpressure_polls = 0;
  bool backpressure_dropped = false;

  if (osd_stats_enabled) {
    const uint32_t ms = (uint32_t)(submit_start_us / 1000);
    const uint32_t prior = __atomic_load_n(&osd_stats.last_input_ms, __ATOMIC_RELAXED);
    if (__atomic_load_n(&osd_stats.input_frames, __ATOMIC_RELAXED)) {
      const uint32_t gap = ms - prior;
      if (gap > 50) __atomic_fetch_add(&osd_stats.input_gaps_50ms, 1, __ATOMIC_RELAXED);
      if (gap > 100) __atomic_fetch_add(&osd_stats.input_gaps_100ms, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&osd_stats.last_input_ms, ms, __ATOMIC_RELAXED);
    __atomic_fetch_add(&osd_stats.input_frames, 1, __ATOMIC_RELAXED);
    /* common-c owns the enqueue epoch; do not subtract our own clock from it. */
    const uint64_t now = LiGetMicroseconds();
    if (decodeUnit->enqueueTimeUs && now >= decodeUnit->enqueueTimeUs &&
        now - decodeUnit->enqueueTimeUs <= 60000000) {
      const uint32_t wait = (uint32_t)(now - decodeUnit->enqueueTimeUs);
      __atomic_fetch_add(&osd_stats.queue_wait_us, wait, __ATOMIC_RELAXED);
      __atomic_fetch_add(&osd_stats.queue_wait_samples, 1, __ATOMIC_RELAXED);
      if (wait > 20000) __atomic_fetch_add(&osd_stats.queue_wait_20ms, 1, __ATOMIC_RELAXED);
    }
  }

  ensure_buf_size(&pkt_buf, &pkt_buf_size, decodeUnit->fullLength);

  int written = 0, length = 0, errCounter = 0, api;
  PLENTRY entry = decodeUnit->bufferList;
  do {
    memcpy(pkt_buf+length, entry->data, entry->length);
    length += entry->length;
    entry = entry->next;
  } while (entry != NULL);

  if (!aml_wait_for_vbuf_room((size_t)length, &backpressure_polls)) {
    backpressure_dropped = true;
    fprintf(stderr,
            "AML: decoder input still above %u%% after %u ms, unit of %d bytes dropped\n",
            AML_VBUF_HIGH_WATER_PERCENT, AML_VBUF_WAIT_TIMEOUT_US / 1000U, length);
  }

  const uint64_t backpressure_wait_us =
      (uint64_t)backpressure_polls * AML_VBUF_POLL_INTERVAL_US;
  uint64_t write_start_us = 0;

  if (!backpressure_dropped) {
    /* Preserve Moonlight's full microsecond timestamp. CoreELEC Kodi and
     * Skupi both use the uS64 ioctl for ES video rather than truncating to ms. */
    (void)codec_checkin_pts_us64(&codecParam, decodeUnit->presentationTimeUs);
    write_start_us = stream_profile.enabled ? aml_profile_now_us() : 0;
    while (length > 0) {
      api = codec_write(&codecParam, pkt_buf+written, length);
      if (stream_profile.enabled) write_calls++;
      if (api < 0) {
        if (errno != EAGAIN) {
          fprintf(stderr, "codec_write() error: %x %d\n", errno, api);
          codec_reset(&codecParam);
          break;
        } else {
          if (stream_profile.enabled) eagain_count++;
          if (osd_stats_enabled) __atomic_fetch_add(&osd_stats.codec_eagain, 1, __ATOMIC_RELAXED);
          if (++errCounter == MAX_WRITE_ATTEMPTS) {
            fprintf(stderr, "codec_write() timeout\n");
            break;
          }
          usleep(EAGAIN_SLEEP_TIME);
        }
      } else {
        written += api;
        length -= api;
      }
    }
  }

  if (!backpressure_dropped && length == 0)
    __atomic_fetch_add(&pacing_units_written, 1, __ATOMIC_RELAXED);

  const bool decoder_error_needs_idr =
      !backpressure_dropped && length == 0 && aml_vdec_error_needs_idr();

  if (osd_stats_enabled) {
    if (!backpressure_dropped) {
      __atomic_fetch_add(&osd_stats.submitted, 1, __ATOMIC_RELAXED);
      if (decodeUnit->frameType == FRAME_TYPE_IDR)
        __atomic_fetch_add(&osd_stats.idr_frames, 1, __ATOMIC_RELAXED);
    }
    if (backpressure_polls) {
      __atomic_fetch_add(&osd_stats.backpressure_waits, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&osd_stats.backpressure_wait_us,
                         (uint32_t)backpressure_wait_us, __ATOMIC_RELAXED);
    }
    if (backpressure_dropped)
      __atomic_fetch_add(&osd_stats.backpressure_drops, 1, __ATOMIC_RELAXED);
    const uint64_t end_us = aml_profile_now_us();
    if (end_us >= submit_start_us) {
      const uint64_t duration = end_us - submit_start_us;
      if (duration > 20000) __atomic_fetch_add(&osd_stats.submit_slow_20ms, 1, __ATOMIC_RELAXED);
      if (duration > 50000) __atomic_fetch_add(&osd_stats.submit_slow_50ms, 1, __ATOMIC_RELAXED);
    }
    if (end_us >= submit_start_us && end_us - submit_start_us <= 1000000) {
      __atomic_fetch_add(&osd_stats.codec_submit_us,
                         (uint32_t)(end_us - submit_start_us), __ATOMIC_RELAXED);
      __atomic_fetch_add(&osd_stats.codec_submit_samples, 1, __ATOMIC_RELAXED);
    }
    if (length && !backpressure_dropped)
      __atomic_fetch_add(&osd_stats.submit_failures, 1, __ATOMIC_RELAXED);
    if (decodeUnit->frameHostProcessingLatency) {
      __atomic_fetch_add(&osd_stats.host_tenths_ms, decodeUnit->frameHostProcessingLatency,
                         __ATOMIC_RELAXED);
      __atomic_fetch_add(&osd_stats.host_samples, 1, __ATOMIC_RELAXED);
    }
    if (decodeUnit->enqueueTimeUs >= decodeUnit->receiveTimeUs) {
      uint64_t dt = decodeUnit->enqueueTimeUs - decodeUnit->receiveTimeUs;
      if (dt <= 1000000) {
        __atomic_fetch_add(&osd_stats.assembly_us, (uint32_t)dt, __ATOMIC_RELAXED);
        __atomic_fetch_add(&osd_stats.assembly_samples, 1, __ATOMIC_RELAXED);
      }
    }
  }
  if (stream_profile.enabled) {
    const uint64_t end_us = aml_profile_now_us();
    AmlProfileSample sample = {
      .bytes = decodeUnit->fullLength,
      // Both timestamps come from moonlight-common-c's own monotonic epoch.
      .assembly_us = decodeUnit->enqueueTimeUs >= decodeUnit->receiveTimeUs ?
          decodeUnit->enqueueTimeUs - decodeUnit->receiveTimeUs : 0,
      .submit_us = end_us >= submit_start_us ? end_us - submit_start_us : 0,
      .write_us = write_start_us && end_us >= write_start_us ?
          end_us - write_start_us : 0,
      .eagain = eagain_count,
      .write_calls = write_calls,
      .backpressure_wait_us = backpressure_wait_us,
      .backpressure_polls = backpressure_polls,
      .failed = length != 0,
      .dropped = backpressure_dropped
    };
    aml_profile_record(&stream_profile, &sample);
  }
  return (length || decoder_error_needs_idr) ? DR_NEED_IDR : DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_aml = {
  .setup = aml_setup,
  .cleanup = aml_cleanup,
  .submitDecodeUnit = aml_submit_decode_unit,

  // We may delay in aml_submit_decode_unit() for a while, so we can't set CAPABILITY_DIRECT_SUBMIT
  .capabilities = CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC,
};
