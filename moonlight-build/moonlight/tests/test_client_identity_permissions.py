"""Compile the installed-credential protection routine with controlled files."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = r'''
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CERTIFICATE_FILE_NAME "client.pem"
#define KEY_FILE_NAME "key.pem"
#define P12_FILE_NAME "client.p12"
#define GS_OK 0
#define GS_FAILED -1
const char *gs_error;
'''
FOOTER = r'''
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  int rc = protect_client_identity(argv[1]);
  if (rc != GS_OK)
    fprintf(stderr, "credential: %s\n", gs_error ? gs_error : "unknown");
  return rc == GS_OK ? 0 : 1;
}
'''


class CredentialPermissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.work = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.work.cleanup)
        folder = Path(cls.work.name)
        source = (ROOT / 'libgamestream/client.c').read_text()
        begin = source.index('static int protect_client_identity(')
        end = source.index('\nint gs_init(', begin)
        (folder / 'test.c').write_text(HEADER + source[begin:end] + FOOTER)
        cls.binary = folder / 'test'
        result = subprocess.run(
            ['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
             str(folder / 'test.c'), '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def test_existing_private_files_are_restricted(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / 'credentials'
            root.mkdir(mode=0o755)
            root.chmod(0o755)
            for name in ('client.pem', 'key.pem', 'client.p12'):
                p = root / name
                p.write_bytes(b'test')
                p.chmod(0o644)
            result = subprocess.run([str(self.binary), str(root)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(root.stat().st_mode & 0o777, 0o700)
            for name in ('client.pem', 'key.pem', 'client.p12'):
                self.assertEqual((root / name).stat().st_mode & 0o777, 0o600)

    def test_symlinked_client_key_is_not_followed(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / 'credentials'
            root.mkdir()
            outside = Path(td) / 'external.key'
            outside.write_bytes(b'existing')
            outside.chmod(0o644)
            (root / 'key.pem').symlink_to(outside)
            result = subprocess.run([str(self.binary), str(root)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn('cannot be secured', result.stderr)
            self.assertEqual(outside.stat().st_mode & 0o777, 0o644)


if __name__ == '__main__':
    unittest.main()
