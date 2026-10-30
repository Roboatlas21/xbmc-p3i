"""Compile the exact libevdev initializer with injected allocation/FD failures."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EvdevOpenTests(unittest.TestCase):
    def test_bad_readable_input_does_not_register_or_leak_fd(self):
        source = (ROOT / 'src/input/evdev.c').read_text()
        start = source.index('static struct libevdev *evdev_open_checked(')
        end = source.index('\n}\n', start) + 3
        implementation = source[start:end]
        self.assertEqual(source.count('evdev_open_checked(fd, device)'), 2)
        mapping_open = source.split('bool evdev_map(char* device)', 1)[1].split(
            'evdev_open_checked(fd, device)', 1)[0]
        self.assertIn('if (fd < 0)', mapping_open)

        harness = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct libevdev { int sentinel; };
static int fail_allocate, fail_bind, freed;
static struct libevdev *libevdev_new(void) {
  if (fail_allocate) return NULL;
  struct libevdev *dev = malloc(sizeof(*dev));
  assert(dev);
  dev->sentinel = 1234;
  return dev;
}
static int libevdev_set_fd(struct libevdev *dev, int fd) {
  assert(dev && dev->sentinel == 1234 && fd >= 0);
  return fail_bind ? -ENOTTY : 0;
}
static void libevdev_free(struct libevdev *dev) {
  assert(dev && dev->sentinel == 1234);
  ++freed;
  free(dev);
}
'''
        harness += implementation
        harness += r'''
int main(void) {
  char path[] = "/tmp/moonlight-evdev-test-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  unlink(path);
  fail_allocate = 1;
  assert(evdev_open_checked(fd, "bad device") == NULL);
  assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
  assert(freed == 0);

  /* Use an existing real (but non-evdev) readable file to model ENOTTY. */
  fd = open("/dev/null", O_RDONLY);
  assert(fd >= 0);
  fail_allocate = 0;
  fail_bind = 1;
  assert(evdev_open_checked(fd, "/dev/null") == NULL);
  assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
  assert(freed == 1);

  fd = open("/dev/null", O_RDONLY);
  assert(fd >= 0);
  fail_bind = 0;
  struct libevdev *dev = evdev_open_checked(fd, "/dev/null");
  assert(dev && fcntl(fd, F_GETFD) >= 0);
  libevdev_free(dev);
  close(fd);
  assert(freed == 2);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'test.c').write_text(harness)
            command = shlex.split(os.environ.get('CC', 'cc'))
            command += ['-std=gnu99', '-Wall', '-Wextra', '-Werror',
                        str(root / 'test.c'), '-o', str(root / 'test')]
            subprocess.run(command, check=True)
            subprocess.run([str(root / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
