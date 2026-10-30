"""Compile the real PCM logging helper to catch callback-level log floods."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AlsaErrorLoggingTests(unittest.TestCase):
    def test_many_errors_produce_at_most_one_line_per_interval(self):
        source = (ROOT / 'src/audio/alsa.c').read_text()
        start = source.index('static void log_incomplete_pcm_write(')
        end = source.index('\n}\n', start) + 3
        implementation = source[start:end]
        harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <limits.h>
static uint64_t fake_now = 10000000ULL;
static uint64_t audio_ticks_us(void) { return fake_now; }
static uint64_t last_pcm_error_log_us;
static unsigned int suppressed_pcm_error_logs;
static bool logged_pcm_error;
'''
        harness += implementation
        harness += r'''
int main(void) {
  for (int i = 0; i < 1000; ++i)
    log_incomplete_pcm_write(120, 240, -11);
  fake_now += 5000000ULL;
  log_incomplete_pcm_write(120, 240, -11);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'log.c').write_text(harness)
            command = shlex.split(os.environ.get('CC', 'cc'))
            command += ['-std=gnu99', '-Wall', '-Wextra', '-Werror',
                        str(path / 'log.c'), '-o', str(path / 'log')]
            subprocess.run(command, check=True)
            result = subprocess.run([str(path / 'log')], check=True,
                                    capture_output=True, text=True)
        lines = [line for line in result.stderr.splitlines()
                 if line.startswith('ALSA: incomplete PCM write')]
        self.assertEqual(len(lines), 2, result.stderr)
        self.assertIn('suppressed=999', lines[-1])


if __name__ == '__main__':
    unittest.main()
