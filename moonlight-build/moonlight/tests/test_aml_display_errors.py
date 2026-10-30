"""Inject decoder faults into the production balanced display loop."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AmlDisplayErrorTests(unittest.TestCase):
    def test_empty_queue_breaks_error_streak_in_real_display_loop(self):
        source = (ROOT / 'src/video/aml.c').read_text()
        display = source[source.index('void* aml_display_thread('):source.index('\nint aml_setup(')]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <linux/videodev2.h>
#include "aml_pacing.h"
#include "aml_pacing_fault.h"
static int done, videoFd, stream_profile, aborted;
static unsigned int next_error, error_count;
static const int *errors;
static uint64_t pacing_frames_dropped, pacing_frames_returned;
static struct { int cntl_handle; } codecParam;
static int fake_ioctl(int fd, unsigned long request, struct v4l2_buffer *buffer) {
  if (next_error == error_count) { done=1; errno=EAGAIN; }
  else errno=errors[next_error++];
  return -1;
}
static int fake_poll(struct pollfd *fds, unsigned long count, int timeout) { return 0; }
static void aml_profile_display_poll(void *profile, bool ready) {}
static void aml_abort_balanced_pacing(const char *operation, int error) { aborted=1; }
static void aml_record_dequeue(void) {}
static int aml_qbuf_index(unsigned int index, bool drop) { return 0; }
#define ioctl fake_ioctl
#define poll fake_poll
'''
        harness += '\n'.join(line for line in source.splitlines()
                             if line.startswith('#define AML_PACING_')) + '\n'
        harness += display + r'''
static void run(const int *sequence, unsigned int length, int expected_abort) {
  done=aborted=0; next_error=0; errors=sequence; error_count=length;
  aml_display_thread(NULL);
  assert(aborted==expected_abort);
}
int main(void) {
  const int separated[]={EIO,EAGAIN,EIO,EAGAIN,EIO};
  const int consecutive[]={EIO,EIO,EIO};
  const int interrupted[]={EIO,EINTR,EIO,EINTR,EIO};
  const int removed[]={ENODEV};
  run(separated,5,0); run(consecutive,3,1);
  run(interrupted,5,1); run(removed,1,1);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'display.c').write_text(harness)
            command = shlex.split(os.environ.get('CC', 'cc'))
            command += ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-I' + str(ROOT / 'src/video'), str(path / 'display.c'), '-o', str(path / 'display')]
            subprocess.run(command, check=True)
            subprocess.run([str(path / 'display')], check=True)


if __name__ == '__main__':
    unittest.main()
