"""Run production AML setup/submission with simulated hardware and clock."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'src/video/aml.c').read_text()


def compile_run(code):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        (path / 'test.c').write_text(code)
        cmd = shlex.split(os.environ.get('CC', 'cc'))
        cmd += ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/video'),
                '-I' + str(ROOT / 'third_party/moonlight-common-c/src'),
                str(path / 'test.c'), '-o', str(path / 'test')]
        subprocess.run(cmd, check=True)
        subprocess.run([str(path / 'test')], check=True, timeout=10)


COMMON = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <Limelight.h>
#include "video/video.h"
#include "aml_profile.h"
#include "osd/stats.h"
static AmlProfile stream_profile;
static AmlFrameStats osd_stats;
static bool osd_stats_enabled, submit_stalling, submit_failed;
static uint64_t submit_stall_since_us;
static uint32_t pacing_units_written;
static void *pkt_buf;
static size_t pkt_buf_size;
static void ensure_buf_size(void **p, size_t *size, size_t needed) {
  if (*size < needed) { *p = realloc(*p, needed); assert(*p); *size = needed; }
}
'''
DEFINES = '\n'.join(line for line in SOURCE.splitlines()
                    if line.startswith('#define ')) + '\n'


class AmlStartupStallTests(unittest.TestCase):
    def test_frame_mode_resume_and_setup_failure_cleanup(self):
        setup = SOURCE[SOURCE.index('int aml_setup('):SOURCE.index('\nvoid aml_cleanup(')]
        compile_run(COMMON + DEFINES + r'''
/* Match enum semantics in pinned libamcodec 8a779ca (not #define values). */
enum { STREAM_TYPE_SINGLE = 0, STREAM_TYPE_FRAME, STREAM_TYPE_STREAM };
enum { STREAM_TYPE_ES_VIDEO = 1, FRAME_BASE_PATH_AMLVIDEO_AMVIDEO = 2,
       FRAME_BASE_PATH_TUNNEL_MODE = 7, VFORMAT_H264, VFORMAT_H264_4K2K,
       VFORMAT_HEVC, VIDEO_DEC_FORMAT_H264, VIDEO_DEC_FORMAT_H264_4K2K,
       VIDEO_DEC_FORMAT_HEVC };
static struct {
  int handle, cntl_handle, audio_utils_handle, sub_handle, has_video, noblock;
  int stream_type, dec_mode, video_path, video_type;
  struct { void *param; int format, width, height, rate; } am_sysinfo;
} codecParam;
static pthread_t displayThread;
static bool balanced_pacing, done, display_thread_started, codec_initialized;
static int videoFd, stream_low_latency, stream_tuning;
static uint32_t last_dequeue_ms, last_vdec_error_count;
static uint64_t last_vdec_error_idr_ms;
static uint32_t pacing_frames_returned, pacing_frames_dropped;
static int stage, fail_stage, paused, opened, closed, restored, thread_started;
static int fake_init(void) {
  assert(codecParam.dec_mode == STREAM_TYPE_FRAME);
  assert(codecParam.video_path == (balanced_pacing ? 2 : 7));
  assert(stage == 0); stage = 1; codecParam.cntl_handle = 11; return 0;
}
static int fake_pause(void) { assert(stage == 1); stage = 2; paused = 1; return 0; }
static int fake_freerun(void) { assert(stage == 2); stage = 3; return fail_stage == 3 ? -1 : 0; }
static int fake_resume(void) { assert(stage == 3); stage = 4; paused = 0; return fail_stage == 4 ? -1 : 0; }
static int fake_thread(void) { assert(!paused && stage == 4); ++thread_started; return 0; }
#define open(...) (++opened, 10)
#define close(fd) ((void)--opened)
#define codec_init(...) fake_init()
#define codec_pause(...) fake_pause()
#define codec_resume(...) fake_resume()
#define codec_close(...) ((void)++closed)
#define codec_set_freerun_mode(...) fake_freerun()
#define codec_set_cntl_mode(...) 0
#define codec_set_video_delay_limited_ms(...) 0
#define codec_set_cntl_avthresh(...) 0
#define codec_set_cntl_syncthresh(...) 0
#define pthread_create(t,...) ((void)(t), fake_thread())
#define aml_profile_init(p,...) ((void)(p))
#define aml_low_latency_init(p,...) ((void)(p))
#define aml_low_latency_enable(...) 0
#define aml_tuning_init(p,...) ((void)(p))
#define aml_tuning_configure(...) ((void)0)
#define aml_tuning_restore(...) (++restored)
#define aml_low_latency_restore(...) (++restored)
''' + setup + r'''
int main(void) {
  for (int balanced = 0; balanced < 2; ++balanced) {
    setenv("MOONLIGHT_FRAME_PACING", balanced ? "balanced" : "latency", 1);
    for (int failure = 0; failure < 3; ++failure) {
      fail_stage = failure ? failure + 2 : 0;
      stage = paused = opened = closed = restored = thread_started = 0;
      submit_stalling = submit_failed = true;
      submit_stall_since_us = 0;
      int rc = aml_setup(VIDEO_FORMAT_H265, 3840, 2160, 60, NULL, 0);
      assert(!submit_stalling && !submit_failed);
      if (!failure) {
        assert(rc == 0 && codec_initialized && !paused && stage == 4);
        assert(thread_started == balanced && closed == 0);
      } else {
        assert(rc < 0 && !codec_initialized && opened == 0);
        assert(closed == 1 && restored == 2 && thread_started == 0);
      }
    }
  }
  free(pkt_buf);
  return 0;
}
''')

    def test_real_submission_bounds_stalls_and_resets_on_progress(self):
        start = SOURCE.index('static void aml_record_submit_progress(')
        progress = SOURCE[start:SOURCE.index('\nstatic uint32_t pacing_units_written;', start)]
        submit = SOURCE[SOURCE.index('int aml_submit_decode_unit('):SOURCE.index('\nDECODER_RENDERER_CALLBACKS decoder_callbacks_aml =')]
        compile_run(COMMON + DEFINES + r'''
#include "aml_write_progress.h"
#include "aml_decode_unit_validate.h"
static uint64_t now;
static int failures, blocked, write_kind, writes;
static void fail(void) { ++failures; }
static void (*aml_failure_callback)(void) = fail;
uint64_t LiGetMicroseconds(void) { return now; }
uint64_t aml_profile_now_us(void) { return now; }
void aml_profile_record(AmlProfile *p, const AmlProfileSample *s) {}
static bool aml_wait_for_vbuf_room(size_t incoming, unsigned int *polls) { return !blocked; }
static bool aml_vdec_error_needs_idr(void) { return false; }
static int fake_write(int length) {
  ++writes;
  if (write_kind == 0) return length;
  if (write_kind == 2 && length > 1) return length - 1;
  errno = EAGAIN; return -1;
}
#define codec_write(p,b,length) fake_write(length)
#define codec_checkin_pts_us64(...) 0
#define codec_reset(...) ((void)0)
#define usleep(...) ((void)0)
''' + progress + submit + r'''
int main(void) {
  char bytes[4] = {0};
  LENTRY entry = {.data=bytes, .length=sizeof(bytes), .next=NULL};
  DECODE_UNIT unit = {.fullLength=sizeof(bytes), .bufferList=&entry};
  for (int mode = 0; mode < 3; ++mode) {
    /* Full queue, EAGAIN, and partial writes followed by EAGAIN. */
    blocked = mode == 0; write_kind = mode;
    now = 0; submit_stalling = submit_failed = false; failures = 0;
    assert(aml_submit_decode_unit(&unit) == DR_NEED_IDR);
    now = 9999999;
    assert(aml_submit_decode_unit(&unit) == DR_NEED_IDR && failures == 0);
    now = 10000000;
    assert(aml_submit_decode_unit(&unit) == DR_NEED_IDR && failures == 1);
    int prior_writes = writes;
    aml_submit_decode_unit(&unit);
    assert(failures == 1 && writes == prior_writes);
  }
  submit_stalling = submit_failed = false; failures = 0;
  for (int i = 0; i < 3; ++i) {
    now += 1; blocked = 1;
    assert(aml_submit_decode_unit(&unit) == DR_NEED_IDR);
    now += 9000000; blocked = 0; write_kind = 0;
    assert(aml_submit_decode_unit(&unit) == DR_OK);
    assert(!submit_stalling && !submit_failed && failures == 0);
  }
  free(pkt_buf);
  return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
