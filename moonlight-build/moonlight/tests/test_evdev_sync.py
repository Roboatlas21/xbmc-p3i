"""Exercise the real event reader through SYN_DROPPED and recovery."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EvdevSyncTests(unittest.TestCase):
    def test_resynchronization_emits_corrected_input_and_resumes(self):
        text = (ROOT / 'src/input/evdev.c').read_text()
        start = text.index('static int evdev_handle(int fd) {')
        end = text.index('\n}\n', start) + 3
        source = text[start:end]
        harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define LOOP_OK 0
#define LOOP_RETURN 1
#define LOOP_ERROR -1
#define LIBEVDEV_READ_STATUS_SUCCESS 0
#define LIBEVDEV_READ_STATUS_SYNC 1
#define LIBEVDEV_READ_FLAG_NORMAL 0
#define LIBEVDEV_READ_FLAG_SYNC 1
struct input_event { int code, value; };
struct input_device { int fd; void *dev; };
static struct input_device only = {10, (void *)1};
static struct input_device *devices[] = {&only};
static int numDevices = 1;
static int reads, events, removed, fail_reads;
static int next_event(void *dev, int flags, struct input_event *ev) {
  assert(dev == (void *)1);
  if (fail_reads) return -EIO;
  ++reads;
  switch (reads) {
  case 1: assert(flags == 0); ev->code = 0; return 1;
  case 2: assert(flags == 1); ev->code = 8; ev->value = 0; return 1;
  case 3: assert(flags == 1); return -EAGAIN;
  case 4: assert(flags == 0); ev->code = 9; ev->value = 1; return 0;
  case 5: assert(flags == 0); return -EAGAIN;
  default: abort();
  }
}
#define libevdev_next_event next_event
static bool record(struct input_event *ev, struct input_device *dev) {
  assert(dev == &only);
  if (events == 0) { assert(ev->code == 8 && ev->value == 0); }
  if (events == 1) { assert(ev->code == 9 && ev->value == 1); }
  ++events;
  return true;
}
static bool (*handler)(struct input_event *, struct input_device *) = record;
static void evdev_remove(int idx) { assert(idx == 0); ++removed; }
'''
        harness += source
        harness += r'''
int main(void) {
  assert(evdev_handle(10) == LOOP_OK);
  assert(reads == 5 && events == 2 && removed == 0);
  fail_reads = 1;
  assert(evdev_handle(10) == LOOP_ERROR);
  assert(removed == 0);
  return 0;
}
'''
        harness = harness.replace('#include <string.h>',
                                  '#include <string.h>\n#include <stdlib.h>')
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / 'test.c').write_text(harness)
            command = shlex.split(os.environ.get('CC', 'cc'))
            command += ['-std=gnu99', '-Wall', '-Wextra', '-Werror',
                        str(root / 'test.c'), '-o', str(root / 'test')]
            subprocess.run(command, check=True)
            subprocess.run([str(root / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
