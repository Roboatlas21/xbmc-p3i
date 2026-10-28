/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Opt-in SDR prototype: FFmpeg V4L2-M2M -> DRM PRIME -> CE21 VFM bridge.
 * This is a separate dlopen module: the default AML backend never loads FFmpeg.
 */
#define _POSIX_C_SOURCE 200809L
#include "video.h"
#include "aml_vfm_frame.h"
#include "../../kernel/moonlight-vfm/moonlight_vfm.h"
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>

static AVCodecContext *decoder;
static AVFrame *inflight[MLVFM_SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool worker_started, stopping, await_idr;
static pthread_cond_t work_ready = PTHREAD_COND_INITIALIZER;
#define INPUT_QUEUE_SIZE 4
static AVPacket *input_queue[INPUT_QUEUE_SIZE];
static unsigned int input_count;
static void *decode_worker(void *unused);
static int bridge_fd = -1, frame_duration;
static void (*request_stop)(void);
static unsigned long submitted, returned, dropped;

/* Does not open devices, register a provider, or change display state. */
int moonlight_vfm_init(void (*stop_callback)(void))
{
  const char *names[] = { "h264_v4l2m2m", "hevc_v4l2m2m" };
  unsigned int n;
  request_stop = stop_callback;
  for (n = 0; n < sizeof(names) / sizeof(names[0]); ++n) {
    const AVCodec *codec = avcodec_find_decoder_by_name(names[n]);
    int i;
    if (!codec) {
      fprintf(stderr, "AML-VFM: FFmpeg is missing %s\n", names[n]);
      return -1;
    }
    AVCodecContext *probe = avcodec_alloc_context3(codec);
    if (!probe || !av_opt_find(probe->priv_data, "nonblocking_dequeue", NULL, 0, 0)) {
      avcodec_free_context(&probe);
      fprintf(stderr, "AML-VFM: %s lacks nonblocking dequeue support\n", names[n]);
      return -1;
    }
    avcodec_free_context(&probe);
    for (i = 0; ; ++i) {
      const AVCodecHWConfig *hw = avcodec_get_hw_config(codec, i);
      if (!hw) {
        fprintf(stderr, "AML-VFM: %s lacks the V4L2 DRM PRIME patch\n", names[n]);
        return -1;
      }
      if (hw->pix_fmt == AV_PIX_FMT_DRM_PRIME)
        break;
    }
  }
  return 0;
}
static void fail(const char *operation, int error)
{
  char detail[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(error, detail, sizeof(detail));
  fprintf(stderr, "AML-VFM: %s: %s; stopping experimental pipeline\n", operation, detail);
  pthread_mutex_lock(&lock);
  stopping = true;
  pthread_mutex_unlock(&lock);
  if (request_stop)
    request_stop();
}
static enum AVPixelFormat select_format(AVCodecContext *ctx,
                                       const enum AVPixelFormat *formats)
{
  for (; *formats != AV_PIX_FMT_NONE; ++formats)
    if (*formats == AV_PIX_FMT_DRM_PRIME)
      return *formats;
  fprintf(stderr, "AML-VFM: decoder offered no DRM PRIME output\n");
  return AV_PIX_FMT_NONE; /* Never quietly switch to CPU decoding/copying. */
}
static int reap_frames(void)
{
  struct mlvfm_done done;
  while (ioctl(bridge_fd, MLVFM_REAP, &done) == 0) {
    if (done.cookie >= MLVFM_SLOTS || !inflight[done.cookie])
      return AVERROR(EPROTO);
    av_frame_free(&inflight[done.cookie]);
    ++returned;
    dropped += !!done.dropped;
  }
  return errno == EAGAIN || errno == EINTR ? 0 : AVERROR(errno);
}
static void vfm_stop(void)
{
  pthread_mutex_lock(&lock);
  stopping = true;
  pthread_cond_signal(&work_ready);
  pthread_mutex_unlock(&lock);
}
static void vfm_cleanup(void)
{
  unsigned int i;
  vfm_stop();
  if (worker_started) {
    pthread_join(worker, NULL);
    worker_started = false;
  }
  for (i = 0; i < input_count; ++i)
    av_packet_free(&input_queue[i]);
  input_count = 0;
  /* close() invokes kernel-side blackout/unregister, including on process
   * death. Do this before releasing FFmpeg frames or decoder capture pools. */
  if (bridge_fd >= 0) {
    close(bridge_fd);
    bridge_fd = -1;
  }
  for (i = 0; i < MLVFM_SLOTS; ++i)
    av_frame_free(&inflight[i]);
  avcodec_free_context(&decoder);
  fprintf(stderr, "AML-VFM: submitted=%lu returned=%lu queue_drops=%lu\n",
          submitted, returned, dropped);
}
static int vfm_setup(int format, int width, int height, int fps,
                     void *context, int flags)
{
  struct mlvfm_info info;
  struct mlvfm_start start = { .abi = MLVFM_ABI, .pending_frames = 1 };
  FILE *blackout;
  unsigned int policy;
  AVDictionary *options = NULL;
  const AVCodec *codec;
  const char *pacing = getenv("MOONLIGHT_FRAME_PACING");
  int ret;
  if ((format != VIDEO_FORMAT_H264 && format != VIDEO_FORMAT_H265) ||
      (flags & DISPLAY_ROTATE_MASK) || width < 2 || height < 2 ||
      width > 4096 || height > 2160 || (width & 1) || (height & 1) ||
      fps < 1 || fps > 120) {
    fprintf(stderr, "AML-VFM: prototype supports unrotated H.264/HEVC SDR, up to 4096x2160/120\n");
    return -1;
  }
  submitted = returned = dropped = 0;
  stopping = false;
  await_idr = false;
  input_count = 0;
  frame_duration = 96000 / fps;
  if (pacing && strcmp(pacing, "balanced") == 0)
    start.pending_frames = 2;
  blackout = fopen("/sys/class/video/blackout_policy", "r");
  if (!blackout) {
    fprintf(stderr, "AML-VFM: cannot snapshot video blackout policy\n");
    goto error;
  }
  ret = fscanf(blackout, "%u", &policy);
  fclose(blackout);
  if (ret != 1 || policy > 1) {
    fprintf(stderr, "AML-VFM: unsupported video blackout policy\n");
    goto error;
  }
  start.blackout_policy = policy;
  bridge_fd = open("/dev/moonlight-vfm", O_RDWR | O_CLOEXEC);
  if (bridge_fd < 0) {
    fprintf(stderr, "AML-VFM: cannot open /dev/moonlight-vfm: %s\n", strerror(errno));
    goto error;
  }
  if (ioctl(bridge_fd, MLVFM_QUERY, &info) < 0 ||
      info.abi != MLVFM_ABI || info.slots != MLVFM_SLOTS) {
    fprintf(stderr, "AML-VFM: bridge ABI mismatch\n");
    goto error;
  }
  codec = avcodec_find_decoder_by_name(format == VIDEO_FORMAT_H264 ?
                                      "h264_v4l2m2m" : "hevc_v4l2m2m");
  decoder = avcodec_alloc_context3(codec);
  if (!decoder || !codec)
    goto error;
  decoder->width = width;
  decoder->height = height;
  decoder->pkt_timebase = (AVRational){ 1, 1000000 };
  decoder->framerate = (AVRational){ fps, 1 };
  decoder->get_format = select_format;
  decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
  decoder->thread_count = 1;
  av_dict_set(&options, "num_output_buffers", "16", 0);
  av_dict_set(&options, "num_capture_buffers", "20", 0);
  av_dict_set(&options, "pixel_format", "nv12", 0);
  av_dict_set(&options, "nonblocking_dequeue", "1", 0);
  ret = avcodec_open2(decoder, codec, &options);
  if (ret < 0 || av_dict_count(options)) {
    fprintf(stderr, "AML-VFM: unable to open patched V4L2 NV12 decoder (error %d, unused options %d)\n",
            ret, av_dict_count(options));
    goto error;
  }
  av_dict_free(&options);
  if (ioctl(bridge_fd, MLVFM_START, &start) < 0) {
    fprintf(stderr, "AML-VFM: cannot register provider: %s\n", strerror(errno));
    goto error;
  }
  if (pthread_create(&worker, NULL, decode_worker, NULL))
    goto error;
  worker_started = true;
  fprintf(stderr, "AML-VFM: experimental SDR pipeline, %u pending frame(s)\n", start.pending_frames);
  return 0;
error:
  av_dict_free(&options);
  vfm_cleanup();
  return -1;
}
static int present(AVFrame *frame)
{
  struct mlvfm_frame output;
  unsigned int i;
  int ret = aml_vfm_frame(frame, decoder->width, decoder->height,
                          frame_duration, &output);
  if (ret < 0)
    return ret;
  pthread_mutex_lock(&lock);
  if (stopping) {
    pthread_mutex_unlock(&lock);
    return AVERROR(EINTR);
  }
  for (i = 0; i < MLVFM_SLOTS && inflight[i]; ++i) {}
  if (i == MLVFM_SLOTS) {
    pthread_mutex_unlock(&lock);
    return AVERROR(ENOBUFS);
  }
  output.cookie = i;
  inflight[i] = av_frame_clone(frame);
  if (!inflight[i]) {
    pthread_mutex_unlock(&lock);
    return AVERROR(ENOMEM);
  }
  ret = ioctl(bridge_fd, MLVFM_SUBMIT, &output);
  if (ret < 0) {
    ret = AVERROR(errno);
    av_frame_free(&inflight[i]);
  } else {
    ++submitted;
  }
  pthread_mutex_unlock(&lock);
  return ret;
}
static int drain_frames(void)
{
  AVFrame *frame = av_frame_alloc();
  int ret;
  if (!frame)
    return AVERROR(ENOMEM);
  while ((ret = avcodec_receive_frame(decoder, frame)) == 0) {
    ret = present(frame);
    av_frame_unref(frame);
    if (ret < 0)
      break;
  }
  av_frame_free(&frame);
  return ret == AVERROR(EAGAIN) ? 0 : ret;
}
static void *decode_worker(void *unused)
{
  AVPacket *pending = NULL;
  for (;;) {
    unsigned int i;
    int ret;
    pthread_mutex_lock(&lock);
    if (stopping) {
      pthread_mutex_unlock(&lock);
      break;
    }
    if (!pending && input_count) {
      pending = input_queue[0];
      for (i = 1; i < input_count; ++i)
        input_queue[i - 1] = input_queue[i];
      --input_count;
    }
    pthread_mutex_unlock(&lock);
    ret = reap_frames();
    if (ret >= 0 && pending) {
      ret = avcodec_send_packet(decoder, pending);
      if (ret >= 0)
        av_packet_free(&pending);
      else if (ret == AVERROR(EAGAIN))
        ret = 0;
    }
    if (ret >= 0)
      ret = drain_frames();
    if (ret < 0) {
      if (ret != AVERROR(EINTR))
        fail("decode/present", ret);
      break;
    }
    /* V4L2 completion is asynchronous. Poll it even when the host produces
     * no new packets. The private FFmpeg option guarantees a zero-timeout
     * capture dequeue; never block the receiver or wait forever during stop. */
    pthread_mutex_lock(&lock);
    if (!stopping && (pending || !input_count)) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 2000000;
      if (until.tv_nsec >= 1000000000) {
        ++until.tv_sec;
        until.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&work_ready, &lock, &until);
    }
    pthread_mutex_unlock(&lock);
  }
  av_packet_free(&pending);
  return NULL;
}
static int vfm_submit(PDECODE_UNIT unit)
{
  AVPacket *packet;
  PLENTRY entry;
  int offset = 0;
  unsigned int i;
  bool stopped;
  pthread_mutex_lock(&lock);
  stopped = stopping;
  pthread_mutex_unlock(&lock);
  if (stopped)
    return DR_OK;
  if (unit->hdrActive || unit->fullLength <= 0 || unit->fullLength > 16 * 1024 * 1024 ||
      unit->presentationTimeUs > INT64_MAX) {
    fail("unsupported decode unit", AVERROR(EINVAL));
    return DR_NEED_IDR;
  }
  packet = av_packet_alloc();
  if (!packet || av_new_packet(packet, unit->fullLength) < 0) {
    av_packet_free(&packet);
    fail("packet allocation", AVERROR(ENOMEM));
    return DR_NEED_IDR;
  }
  for (entry = unit->bufferList; entry; entry = entry->next) {
    if (entry->length < 0 || entry->length > unit->fullLength - offset) {
      av_packet_free(&packet);
      fail("invalid packet chain", AVERROR(EINVAL));
      return DR_NEED_IDR;
    }
    memcpy(packet->data + offset, entry->data, entry->length);
    offset += entry->length;
  }
  if (offset != unit->fullLength) {
    av_packet_free(&packet);
    fail("truncated packet chain", AVERROR(EINVAL));
    return DR_NEED_IDR;
  }
  packet->pts = packet->dts = unit->presentationTimeUs;
  if (unit->frameType == FRAME_TYPE_IDR)
    packet->flags |= AV_PKT_FLAG_KEY;
  pthread_mutex_lock(&lock);
  if (stopping) {
    pthread_mutex_unlock(&lock);
    av_packet_free(&packet);
    return DR_OK;
  }
  if (input_count == INPUT_QUEUE_SIZE) {
    for (i = 0; i < input_count; ++i)
      av_packet_free(&input_queue[i]);
    input_count = 0;
    await_idr = true;
  }
  if (await_idr && unit->frameType != FRAME_TYPE_IDR) {
    pthread_mutex_unlock(&lock);
    av_packet_free(&packet);
    return DR_NEED_IDR;
  }
  await_idr = false;
  input_queue[input_count++] = packet;
  pthread_cond_signal(&work_ready);
  pthread_mutex_unlock(&lock);
  return DR_OK;
}
DECODER_RENDERER_CALLBACKS decoder_callbacks_aml_vfm = {
  .setup = vfm_setup, .stop = vfm_stop, .cleanup = vfm_cleanup,
  .submitDecodeUnit = vfm_submit,
  /* Use common's decoder thread; no direct network-thread decode or RFI. */
  .capabilities = 0,
};
