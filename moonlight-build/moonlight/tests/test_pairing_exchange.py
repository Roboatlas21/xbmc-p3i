"""Exercise gs_pair through a complete Apollo-style HTTP/TLS PIN exchange.

Uses the native harness and system OpenSSL; no Python crypto dependency.
"""
from contextlib import ExitStack
from hashlib import sha256
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import unittest
from urllib.parse import parse_qs, urlsplit

from test_pairing_errors import NativePairingHarness


def openssl(*args, data=None):
    return subprocess.run(['openssl', *map(str, args)], input=data,
                          capture_output=True, check=True).stdout


def certificate_signature(pem):
    # DER Certificate ::= SEQUENCE { tbsCertificate, algorithm, BIT STRING }.
    der = ssl.PEM_cert_to_DER_cert(pem.decode())

    def field(offset):
        length = der[offset + 1]
        start = offset + 2
        if length & 128:
            count = length & 127
            length = int.from_bytes(der[start:start + count], 'big')
            start += count
        return start, start + length

    start, _ = field(0)
    _, end = field(start)
    _, end = field(end)
    start, end = field(end)
    return der[start + 1:end]  # Skip BIT STRING's unused-bits byte.


class PairingExchangeTests(NativePairingHarness, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.host = Path(cls.temp.name)
        openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                '-subj', '/CN=PairingTest', '-keyout', cls.host / 'host.key',
                '-out', cls.host / 'host.pem')
        cls.pem = (cls.host / 'host.pem').read_bytes()

    def exchange(self, challenge_bytes=48, malformed=None, rollback_error=False):
        phases, failures = [], []
        secret, challenge = b'S' * 16, b'C' * 16
        host = self

        class Host(BaseHTTPRequestHandler):
            def do_GET(self):
                try:
                    self.respond()
                except Exception as error:
                    failures.append(error)
                    self.close_connection = True

            def respond(self):
                query = parse_qs(urlsplit(self.path).query)
                phase = next((name for name in ('clientchallenge', 'serverchallengeresp',
                             'clientpairingsecret') if name in query),
                             query.get('phrase', ['unpair'])[0])
                phases.append(phase)
                body = '<paired>1</paired>'
                status = 200
                if phase == 'getservercert':
                    self.server.aes = sha256(bytes.fromhex(query['salt'][0]) + b'1234').digest()[:16]
                    self.server.client = bytes.fromhex(query['clientcert'][0])
                    body += '<plaincert>%s</plaincert>' % host.pem.hex()
                elif phase == 'clientchallenge':
                    decrypt = openssl('enc', '-d', '-aes-128-ecb', '-nopad', '-K',
                                      self.server.aes.hex(), data=bytes.fromhex(query[phase][0]))
                    response = sha256(decrypt + certificate_signature(host.pem) + secret).digest() + challenge
                    response += bytes([16]) * (challenge_bytes - len(response))
                    encrypted = openssl('enc', '-aes-128-ecb', '-nopad', '-K',
                                        self.server.aes.hex(), data=response).hex()
                    body += '<challengeresponse>%s</challengeresponse>' % (
                        malformed if malformed is not None else encrypted)
                elif phase == 'serverchallengeresp':
                    self.server.client_hash = openssl('enc', '-d', '-aes-128-ecb', '-nopad',
                        '-K', self.server.aes.hex(), data=bytes.fromhex(query[phase][0]))
                    if rollback_error:
                        body = '<paired>0</paired>'
                        status = 400
                    else:
                        signature = openssl('dgst', '-sha256', '-sign', host.host / 'host.key', data=secret)
                        body += '<pairingsecret>%s</pairingsecret>' % (secret + signature).hex()
                elif phase == 'clientpairingsecret':
                    client_secret = bytes.fromhex(query[phase][0])[:16]
                    expected = sha256(challenge + certificate_signature(self.server.client) + client_secret).digest()
                    host.assertEqual(self.server.client_hash, expected)
                elif phase == 'pairchallenge':
                    host.assertIsInstance(self.connection, ssl.SSLSocket)
                elif phase == 'unpair':
                    status = 400
                message = 'original challenge failure' if phase == 'serverchallengeresp' else 'rollback failed'
                payload = ('<root status_code="%d" status_message="%s">%s</root>' %
                           (status, message, body)).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_args):
                pass

        with tempfile.TemporaryDirectory() as folder, ExitStack() as stack:
            directory = Path(folder)
            (directory / 'client.pem').write_bytes(self.pem)
            (directory / 'key.pem').write_bytes((self.host / 'host.key').read_bytes())
            servers = [stack.enter_context(ThreadingHTTPServer(('127.0.0.1', 0), Host)) for _ in range(2)]
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(self.host / 'host.pem', self.host / 'host.key')
            servers[1].socket = context.wrap_socket(servers[1].socket, server_side=True)
            for server in servers:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                stack.callback(thread.join)
                stack.callback(server.shutdown)
            result = subprocess.run([str(self.executable), str(servers[0].server_port),
                '123', '0', folder, str(servers[1].server_port)],
                capture_output=True, text=True, timeout=15)
            self.assertEqual(failures, [], str(failures))
            pins = list(directory.glob('server-*.pem'))
            if malformed is None and not rollback_error:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(phases, ['getservercert', 'clientchallenge', 'serverchallengeresp',
                                          'clientpairingsecret', 'pairchallenge'])
                self.assertEqual(len(pins), 1)
                self.assertEqual(pins[0].read_bytes(), self.pem)
            else:
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(phases[-1], 'unpair')
                self.assertEqual(pins, [])
            return result.stdout + result.stderr

    def test_apollo_48_byte_challenge_completes_and_pins_certificate(self):
        self.exchange(48)

    def test_legacy_64_byte_challenge_remains_supported(self):
        self.exchange(64)

    def test_malformed_challenges_are_rejected_without_pinning(self):
        for malformed in ('', 'ab' * 32, 'ab' * 47, 'ab' * 49, 'ab' * 80, 'a' * 95, 'zz' * 48):
            with self.subTest(length=len(malformed)):
                output = self.exchange(malformed=malformed)
                self.assertIn('exchanging the pairing challenge:', output)
                self.assertNotIn('currently in a game', output)
                self.assertNotIn('too big', output)

    def test_rollback_cannot_overwrite_original_xml_error(self):
        output = self.exchange(rollback_error=True)
        self.assertIn('verifying the host pairing secret: original challenge failure', output)
        self.assertNotIn('rollback failed', output)


if __name__ == '__main__':
    unittest.main()
