"""Compile the production PEM reader, including bounds checks, with OpenSSL."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PRELUDE = '''#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#define CERTIFICATE_FILE_NAME "client.pem"
#define KEY_FILE_NAME "key.pem"
#define P12_FILE_NAME "client.p12"
#define GS_OK 0
#define GS_FAILED -1
static X509 *cert;
static char cert_hex[8192];
static EVP_PKEY *privateKey;
const char *gs_error;
typedef int CERT_KEY_PAIR;
static CERT_KEY_PAIR mkcert_generate(void) { abort(); }
static int mkcert_save(const char *a,const char *b,const char *c,CERT_KEY_PAIR p) { abort(); return 0; }
static void mkcert_free(CERT_KEY_PAIR p) { abort(); }
'''
MAIN = '''int main(int argc, char **argv) {
 if (argc != 2) return 2;
 int code = load_cert(argv[1]);
 printf("result=%d message=%s\\n", code, gs_error ? gs_error : "none");
 X509_free(cert); EVP_PKEY_free(privateKey);
 return code == GS_OK ? 0 : 1;
}
'''


class CertificateHexBoundsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc') or not shutil.which('openssl'):
            raise unittest.SkipTest('Host OpenSSL toolchain unavailable')
        cls.work = tempfile.TemporaryDirectory()
        root = Path(cls.work.name)
        text = (ROOT / 'libgamestream/client.c').read_text()
        start = text.index('static int load_cert(')
        end = text.index('\nstatic int load_serverinfo(', start)
        (root / 'harness.c').write_text(PRELUDE + text[start:end] + MAIN)
        cls.executable = root / 'read-cert'
        build = subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra',
                                '-Wno-unused-parameter', '-fsanitize=address,undefined',
                                '-g', str(root / 'harness.c'), '-lssl', '-lcrypto',
                                '-o', str(cls.executable)], capture_output=True, text=True)
        if build.returncode != 0 and 'openssl/' in build.stderr and 'No such file' in build.stderr:
            raise unittest.SkipTest('OpenSSL development headers unavailable')
        if build.returncode != 0:
            raise AssertionError(build.stderr)
        cert = root / 'client.pem'
        key = root / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048',
                        '-nodes', '-days', '1', '-subj', '/CN=MoonlightLocalTest',
                        '-keyout', str(key), '-out', str(cert)], check=True,
                       capture_output=True)
        cls.cert_data = cert.read_bytes()
        cls.key_data = key.read_bytes()

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, 'work'):
            cls.work.cleanup()

    def check_pem_length(self, length, expected):
        with tempfile.TemporaryDirectory() as folder:
            cert = self.cert_data
            self.assertLess(len(cert), length)
            (Path(folder) / 'client.pem').write_bytes(cert + b' ' * (length - len(cert)))
            (Path(folder) / 'key.pem').write_bytes(self.key_data)
            env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
            result = subprocess.run([str(self.executable), folder],
                                    capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, expected, result.stderr)
            self.assertNotIn('AddressSanitizer:', result.stderr)
            return result.stdout

    def test_valid_certificate_and_maximum_supported_file(self):
        self.assertIn('result=0', self.check_pem_length(4095, 0))

    def test_oversized_pem_with_valid_certificate_rejected_safely(self):
        self.assertIn('Certificate file too large', self.check_pem_length(4096, 1))

    def test_corrupt_private_key_fails_at_initialization(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'client.pem').write_bytes(self.cert_data)
            (Path(folder) / 'key.pem').write_text('not a PEM private key')
            result = subprocess.run([str(self.executable), folder],
                                    capture_output=True, text=True,
                                    env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('Client private key is missing', result.stdout)

    def test_private_key_must_match_certificate(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'client.pem').write_bytes(self.cert_data)
            key = Path(folder) / 'key.pem'
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA',
                            '-pkeyopt', 'rsa_keygen_bits:2048', '-out', str(key)],
                           check=True, capture_output=True)
            result = subprocess.run([str(self.executable), folder],
                                    capture_output=True, text=True,
                                    env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('does not match', result.stdout)


if __name__ == '__main__':
    unittest.main()
