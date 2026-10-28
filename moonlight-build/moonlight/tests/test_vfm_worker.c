/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Run the real worker/submit/cleanup code against simulated bridge/decoder I/O.
 * AVPacket/AVFrame allocation and reference counting use the real FFmpeg libs.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdarg.h>
#include <libavcodec/avcodec.h>
#include <sys/ioctl.h>
#include <unistd.h>
static int bridge_ioctl_mock(int fd, unsigned long command, ...);
static int bridge_close_mock(int fd);
static int send_mock(AVCodecContext *ctx, const AVPacket *packet);
static int receive_mock(AVCodecContext *ctx, AVFrame *frame);
#define ioctl bridge_ioctl_mock
#define close bridge_close_mock
#define avcodec_send_packet send_mock
#define avcodec_receive_frame receive_mock
#include "../src/video/aml_vfm.c"
#undef ioctl
#undef close
#undef avcodec_send_packet
#undef avcodec_receive_frame

static int close_calls, stop_calls, released, map_error, reap_error;
static int reap_cookie = -1, send_calls, receive_calls;
static const AVPacket *first_packet;

static void request_stop_mock(void) { ++stop_calls; }
static int bridge_close_mock(int fd) {
  assert(fd == 123); ++close_calls; return 0;
}
static int bridge_ioctl_mock(int fd, unsigned long command, ...) {
  assert(fd == 123);
  va_list args;
  va_start(args, command);
  if (command == MLVFM_SUBMIT) {
    struct mlvfm_frame *frame = va_arg(args, struct mlvfm_frame *);
    assert(frame->width == 1920 && frame->height == 1080);
    va_end(args);
    if (map_error) { errno = map_error; return -1; }
    return 0;
  }
  assert(command == MLVFM_REAP);
  struct mlvfm_done *done = va_arg(args, struct mlvfm_done *);
  va_end(args);
  if (reap_cookie >= 0) {
    done->cookie = reap_cookie; done->dropped = 1; reap_cookie = -1;
    return 0;
  }
  errno = reap_error ? reap_error : EAGAIN;
  return -1;
}
static int send_mock(AVCodecContext *ctx, const AVPacket *packet) {
  assert(ctx == decoder);
  if (++send_calls == 1) {
    first_packet = packet;
    return AVERROR(EAGAIN);
  }
  assert(packet == first_packet && packet->size == 4);
  return 0;
}
static int receive_mock(AVCodecContext *ctx, AVFrame *frame) {
  assert(ctx == decoder);
  (void)frame;
  ++receive_calls;
  if (send_calls == 2) vfm_stop();
  return AVERROR(EAGAIN);
}
static void released_frame(void *opaque, uint8_t *data) {
  if (opaque) assert(close_calls == 1); /* Display detached before last ref. */
  ++released;
  av_free(data);
}
static AVFrame *frame_new(bool require_close) {
  AVFrame *frame = av_frame_alloc();
  AVDRMFrameDescriptor *desc = av_mallocz(sizeof(*desc));
  assert(frame && desc);
  frame->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc),
                                  released_frame, require_close ? (void *)1 : NULL, 0);
  assert(frame->buf[0]);
  frame->data[0] = (uint8_t *)desc;
  frame->width = 1920; frame->height = 1080;
  frame->format = AV_PIX_FMT_DRM_PRIME;
  frame->colorspace = AVCOL_SPC_BT709;
  desc->nb_layers = desc->nb_objects = 1;
  desc->objects[0].fd = 7;
  desc->objects[0].size = 1920 * 1080 * 3 / 2;
  desc->objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
  desc->layers[0].format = DRM_FORMAT_NV12;
  desc->layers[0].nb_planes = 2;
  desc->layers[0].planes[0].pitch = desc->layers[0].planes[1].pitch = 1920;
  desc->layers[0].planes[1].offset = 1920 * 1080;
  return frame;
}
static void reset(void) {
  assert(!decoder && input_count == 0);
  decoder = avcodec_alloc_context3(NULL);
  assert(decoder);
  decoder->width = 1920; decoder->height = 1080;
  bridge_fd = 123; frame_duration = 1600;
  stopping = await_idr = false;
  close_calls = stop_calls = released = map_error = reap_error = 0;
  reap_cookie = -1;
  submitted = returned = dropped = 0;
  send_calls = receive_calls = 0;
  request_stop = request_stop_mock;
}
int main(void) {
  LENTRY entry = { .data = "test", .length = 4 };
  DECODE_UNIT unit = { .fullLength = 4, .bufferList = &entry,
                       .frameType = FRAME_TYPE_PFRAME };
  reset();
  for (int i = 0; i < INPUT_QUEUE_SIZE; ++i) assert(vfm_submit(&unit) == DR_OK);
  assert(vfm_submit(&unit) == DR_NEED_IDR && input_count == 0);
  assert(vfm_submit(&unit) == DR_NEED_IDR && input_count == 0);
  unit.frameType = FRAME_TYPE_IDR;
  assert(vfm_submit(&unit) == DR_OK && input_count == 1);
  decode_worker(NULL); /* send EAGAIN must retain and retry the same packet. */
  assert(send_calls == 2 && receive_calls == 2 && input_count == 0);
  assert(stop_calls == 0);
  vfm_cleanup();

  reset();
  entry.length = 5;
  assert(vfm_submit(&unit) == DR_NEED_IDR && stopping && stop_calls == 1);
  assert(input_count == 0);
  vfm_cleanup();
  entry.length = 4;

  reset();
  AVFrame *frame = frame_new(false);
  map_error = EIO;
  assert(present(frame) == AVERROR(EIO) && inflight[0] == NULL);
  assert(av_buffer_get_ref_count(frame->buf[0]) == 1);
  map_error = 0;
  assert(present(frame) == 0 && av_buffer_get_ref_count(frame->buf[0]) == 2);
  reap_cookie = 0;
  assert(reap_frames() == 0 && returned == 1 && dropped == 1);
  assert(av_buffer_get_ref_count(frame->buf[0]) == 1);
  reap_cookie = MLVFM_SLOTS;
  assert(reap_frames() == AVERROR(EPROTO));
  av_frame_free(&frame);
  assert(released == 1);
  vfm_cleanup();

  reset();
  frame = frame_new(true);
  assert(present(frame) == 0);
  av_frame_free(&frame);
  assert(released == 0);
  reap_error = EIO;
  decode_worker(NULL);
  assert(stopping && stop_calls == 1);
  vfm_cleanup();
  assert(close_calls == 1 && released == 1 && inflight[0] == NULL);
  vfm_cleanup(); /* Repeated cleanup must not close or release twice. */
  assert(close_calls == 1 && released == 1);
  puts("VFM worker: overflow/IDR, EAGAIN retry, bad input, mapping failure, ownership and cleanup passed");
}
