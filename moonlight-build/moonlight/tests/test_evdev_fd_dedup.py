"""Compile the production evdev duplicate-device guard with real file descriptors."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EvdevFdDedupTests(unittest.TestCase):
    def test_alias_to_existing_node_is_ignored_without_skipping_other_nodes(self):
        if not shutil.which('cc'):
            self.skipTest('C compiler unavailable')
        code = (ROOT / 'src/input/evdev.c').read_text()
        start = code.index('static bool evdev_fd_already_registered(int fd) {')
        end = code.index('static void evdev_remove(', start)
        check = code[start:end]
        harness = """
#include <assert.h>
#include <stdbool.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
struct input_device { int fd; };
static struct input_device *devices[2];
static int numDevices;
static pthread_mutex_t devices_mutex = PTHREAD_MUTEX_INITIALIZER;
""" + check + """
int main(int argc, char **argv) {
  assert(argc == 3);
  int first = open(argv[1], O_RDONLY);
  int same = open(argv[1], O_RDONLY);
  int other = open(argv[2], O_RDONLY);
  assert(first >= 0 && same >= 0 && other >= 0);
  struct input_device active = {.fd = first};
  devices[0] = &active;
  numDevices = 1;
  assert(evdev_fd_already_registered(same));
  assert(!evdev_fd_already_registered(other));
  numDevices = 0;
  assert(!evdev_fd_already_registered(same));
  close(first);
  close(same);
  close(other);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'event0').write_text('first')
            (root / 'event1').write_text('second')
            (root / 'dedup.c').write_text(harness)
            compile_result = subprocess.run(
                ['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-pthread',
                 '-fsanitize=undefined', '-fno-sanitize-recover=undefined',
                 str(root / 'dedup.c'), '-o', str(root / 'dedup')],
                text=True, capture_output=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            subprocess.run([str(root / 'dedup'), str(root / 'event0'),
                            str(root / 'event1')], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
