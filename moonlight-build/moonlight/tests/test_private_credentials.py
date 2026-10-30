"""Exercise the real credential writer with a permissive process umask."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

SOURCE = r'''
#include "mkcert.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    umask(0022);
    CERT_KEY_PAIR pair = mkcert_generate();
    if (!pair.x509 || !pair.pkey || !pair.p12) return 3;
    int ok = mkcert_save(argv[1], argv[2], argv[3], pair);
    mkcert_free(pair);
    return ok ? 0 : 1;
}
'''


class CredentialCreationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        root = Path(cls.folder.name)
        harness = root / 'mkcert-test.c'
        harness.write_text(SOURCE)
        cls.executable = root / 'mkcert-test'
        result = subprocess.run(
            ['cc', '-std=gnu99', '-I' + str(ROOT / 'libgamestream'),
             str(harness), str(ROOT / 'libgamestream/mkcert.c'),
             '-lssl', '-lcrypto', '-o', str(cls.executable)],
            capture_output=True, text=True)
        if result.returncode:
            if 'openssl/' in result.stderr and 'No such file' in result.stderr:
                raise unittest.SkipTest('OpenSSL development headers unavailable')
            raise AssertionError(result.stderr)

    def test_permissions_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cert, p12, key = (root / n for n in ('client.pem', 'client.p12', 'key.pem'))
            args = [str(self.executable), str(cert), str(p12), str(key)]
            result = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for path in (key, p12, cert):
                self.assertTrue(path.is_file())
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            before = {p: p.read_bytes() for p in (cert, p12, key)}
            retry = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(retry.returncode, 1)
            self.assertEqual({p: p.read_bytes() for p in before}, before)

    def test_failed_partial_creation_cleans_new_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cert, p12, key = (root / n for n in ('client.pem', 'client.p12', 'key.pem'))
            # An existing P12 must not be replaced; the newly created key
            # from this failed attempt must be cleaned up.
            p12.write_bytes(b'existing')
            result = subprocess.run(
                [str(self.executable), str(cert), str(p12), str(key)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(p12.read_bytes(), b'existing')
            self.assertFalse(key.exists())
            self.assertFalse(cert.exists())


if __name__ == '__main__':
    unittest.main()
