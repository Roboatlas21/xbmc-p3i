"""Compile the actual native CSPRNG pairing-PIN helper against a deterministic stub."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PairingPinTests(unittest.TestCase):
    def test_rejection_sampling_and_rng_failure(self):
        source = (ROOT / 'src/main.c').read_text()
        start = source.index('static bool generate_pairing_pin(')
        end = source.index('\n}\n', start) + 3
        implementation = source[start:end]
        self.assertIn('!generate_pairing_pin(pin)', source)
        self.assertNotIn('random() % 10', source)
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
static int calls, fail_rng;
static int RAND_bytes(unsigned char *buffer, int count) {
  static const unsigned char samples[] = {251, 249, 20, 39, 40};
  if (fail_rng || count != 1) return 0;
  *buffer = samples[calls++ % (sizeof(samples) / sizeof(samples[0]))];
  return 1;
}
'''
        harness += implementation
        harness += r'''
int main(void) {
  char pin[5] = {0};
  assert(generate_pairing_pin(pin));
  assert(strcmp(pin, "9090") == 0);
  assert(calls == 5);
  fail_rng = 1;
  assert(!generate_pairing_pin(pin));
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'pin.c').write_text(harness)
            command = shlex.split(os.environ.get('CC', 'cc'))
            command += ['-std=gnu99', '-Wall', '-Wextra', '-Werror',
                        str(path / 'pin.c'), '-o', str(path / 'pin')]
            subprocess.run(command, check=True)
            subprocess.run([str(path / 'pin')], check=True)


if __name__ == '__main__':
    unittest.main()
