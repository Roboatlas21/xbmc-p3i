import os
import tempfile
import unittest
from pathlib import Path

from resources.lib.util.hostapi import Host, HostError, HostIdentityError, DEFAULT_UNIQUE_ID, HTTPS_PORT


class HostUniqueIdTests(unittest.TestCase):
    def test_exact_persisted_id_is_reused(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'uniqueid.dat').write_bytes(b'FEDCBA9876543210')
            self.assertEqual(Host('host', key_dir=tmp)._unique_id(),
                             'FEDCBA9876543210')

    def test_trailing_bytes_are_ignored_like_native_moonlight(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'uniqueid.dat').write_bytes(b'0123456789ABCDEFextra-data')
            self.assertEqual(Host('host', key_dir=tmp)._unique_id(),
                             '0123456789ABCDEF')

    def test_special_file_and_symlink_never_block_or_change_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, 'uniqueid.dat')
            os.mkfifo(path)
            with self.assertRaises(HostError):
                Host('host', key_dir=directory)._unique_id()
            path.unlink()
            destination = Path(directory, 'other-id')
            destination.write_bytes(b'0123456789ABCDEF')
            path.symlink_to(destination)
            with self.assertRaises(HostError):
                Host('host', key_dir=directory)._unique_id()
            path.unlink()
            path.mkdir()
            with self.assertRaises(HostError):
                Host('host', key_dir=directory)._unique_id()

    def test_only_missing_file_uses_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, 'uniqueid.dat')
            self.assertEqual(Host('host', key_dir=tmp)._unique_id(),
                             DEFAULT_UNIQUE_ID)
            for invalid in (b'too-short', b'123456789012345\xff',
                            b'123456789012345Z'):
                path.write_bytes(invalid)
                with self.assertRaises(HostError):
                    Host('host', key_dir=tmp)._unique_id()
            path.unlink()
            path.symlink_to('missing')
            with self.assertRaises(HostError):
                Host('host', key_dir=tmp)._unique_id()



class TrustedPinPresenceTests(unittest.TestCase):
    def test_only_the_selected_hosts_certificate_is_trusted(self):
        import hashlib
        with tempfile.TemporaryDirectory() as tmp:
            selected = Host('host-A', key_dir=tmp)
            self.assertFalse(selected.has_trusted_pin())
            root = Path(tmp)
            (root / ('server-%s.pem' % hashlib.sha256(b'host-B').hexdigest())).write_text('pin')
            self.assertFalse(selected.has_trusted_pin())
            (root / ('server-%s.pem' % hashlib.sha256(b'host-A').hexdigest())).write_text('pin')
            self.assertTrue(selected.has_trusted_pin())


class PairStatusTests(unittest.TestCase):
    def test_https_context_pins_public_key_not_full_certificate(self):
        import hashlib
        import ssl
        import types
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'client.pem').write_text('client')
            (root / 'key.pem').write_text('key')
            pin = root / ('server-%s.pem' % hashlib.sha256(b'host-A').hexdigest())
            pin.write_text('server cert')
            host = Host('host-A', key_dir=tmp)
            context = types.SimpleNamespace()
            context.load_cert_chain = mock.Mock()
            context.set_ciphers = mock.Mock()
            with mock.patch('resources.lib.util.hostapi.ssl.SSLContext',
                            return_value=context), \
                 mock.patch('resources.lib.util.hostapi.ssl.PEM_cert_to_DER_cert',
                            return_value=b'X' * 128), \
                 mock.patch('resources.lib.util.hostapi.spki_sha256',
                            return_value=b'A' * 32):
                host._context()
            self.assertEqual(host._spki_pin, b'A' * 32)
            self.assertEqual(context.verify_mode, ssl.CERT_NONE)

    def test_revoked_tls_credentials_use_validated_public_fallback(self):
        from unittest import mock
        import ssl
        host = Host('example.test')
        with mock.patch.object(host, '_get', side_effect=ssl.SSLError('revoked')), \
             mock.patch.object(host, '_get_public_serverinfo',
                               return_value=b'<root status_code="200"><PairStatus>0</PairStatus></root>'):
            self.assertFalse(host.pair_status(allow_http_fallback=True))
            self.assertTrue(host._last_pair_status_public)

    def test_invalid_public_status_is_not_treated_as_success(self):
        from unittest import mock
        host = Host('example.test')
        with mock.patch.object(host, '_get', side_effect=HostError('revoked')), \
             mock.patch.object(host, '_get_public_serverinfo',
                               return_value=b'<root status_code="200"/>'):
            with self.assertRaises(HostError):
                host.pair_status(allow_http_fallback=True)

    def test_secure_protocol_rejection_can_use_public_status(self):
        from unittest import mock
        host = Host('example.test')
        with mock.patch.object(host, '_get',
                               return_value=b'<root status_code="401"/>'), \
             mock.patch.object(host, '_get_public_serverinfo',
                               return_value=b'<root status_code="200"><PairStatus>0</PairStatus></root>'):
            self.assertFalse(host.pair_status(allow_http_fallback=True))
            self.assertTrue(host._last_pair_status_public)

    def test_bad_secure_xml_does_not_fall_back_to_public_endpoint(self):
        from unittest import mock
        host = Host('example.test')
        with mock.patch.object(host, '_get', return_value=b'<bad'), \
             mock.patch.object(host, '_get_public_serverinfo') as public:
            with self.assertRaises(HostError):
                host.pair_status(allow_http_fallback=True)
            public.assert_not_called()

    def test_status_is_parsed_not_guessed_from_missing_tag(self):
        host = Host('host')
        cases = [
            (b'<root status_code="200"><PairStatus>0</PairStatus></root>', False),
            (b'<root status_code="200"><PairStatus>1</PairStatus></root>', True),
        ]
        for body, expected in cases:
            host._get = lambda *_args, _body=body: _body
            self.assertIs(host.pair_status(), expected)

    def test_bad_status_response_is_not_treated_as_unpaired(self):
        host = Host('host')
        for body in (b'<root status_code="503"><PairStatus>0</PairStatus></root>',
                     b'<root status_code="200"/>', b'<bad'):
            host._get = lambda *_args, _body=body: _body
            with self.assertRaises(HostError):
                host.pair_status()



class HttpResponseLimitTests(unittest.TestCase):
    def test_no_saved_pin_does_not_create_an_https_connection(self):
        from unittest import mock
        host = Host('host-A')
        with mock.patch.object(host, '_context', return_value=object()), \
             mock.patch('resources.lib.util.hostapi.httplib.HTTPSConnection') as conn:
            with self.assertRaises(HostIdentityError):
                host._get('/applist', [])
            conn.assert_not_called()

    def test_authenticated_xml_rejects_excessive_response(self):
        from unittest import mock
        host = Host('host-A')
        host._spki_pin = b'A' * 32
        class Response:
            status = 200
            def read(self, limit):
                self.limit = limit
                return b'x' * limit
        class Connection:
            response = Response()
            sock = type('Socket', (), {
                'getpeercert': lambda self, binary_form=False: b'peer'})()
            def connect(self):
                pass
            def request(self, *_args):
                pass
            def getresponse(self):
                return self.response
            def close(self):
                pass
        conn = Connection()
        with mock.patch.object(host, '_context', return_value=object()), \
             mock.patch('resources.lib.util.hostapi.spki_sha256',
                        return_value=b'A' * 32), \
             mock.patch('resources.lib.util.hostapi.httplib.HTTPSConnection',
                        return_value=conn):
            with self.assertRaisesRegex(HostError, 'exceeds'):
                host._get('/serverinfo', [])
        self.assertEqual(conn.response.limit, 2 * 1024 * 1024 + 1)

    def test_public_xml_rejects_excessive_response(self):
        from unittest import mock
        host = Host('host-A')
        class Response:
            status = 200
            def read(self, limit):
                return b'x' * limit
        class Connection:
            def request(self, *_args):
                pass
            def getresponse(self):
                return Response()
            def close(self):
                pass
        with mock.patch('resources.lib.util.hostapi.httplib.HTTPConnection',
                        return_value=Connection()):
            with self.assertRaisesRegex(HostError, 'size limit'):
                host._get_public_serverinfo()

class AdvertisedPortTests(unittest.TestCase):
    def test_parse_port_strictly_and_ignore_untrusted_pairing_status(self):
        from unittest import mock
        host = Host('host-A')
        for candidate, expected in (
                ('50000', 50000), ('47984', 47984), ('65535', 65535),
                ('0', None), ('65536', None), ('-1', None),
                ('50000junk', None), ('', None)):
            with self.subTest(candidate=candidate), \
                 mock.patch.object(
                     host, '_get_public_serverinfo',
                     return_value=(
                         '<root status_code="200"><PairStatus>1</PairStatus>'
                         '<HttpsPort>%s</HttpsPort></root>' % candidate
                     ).encode()):
                self.assertEqual(host._advertised_https_port(), expected)

    def test_connect_failure_retries_advertised_port_with_same_spki(self):
        from unittest import mock
        host = Host('host-A')
        host._spki_pin = b'A' * 32
        attempts = []
        requested = []
        class Connection:
            def __init__(self, address, port, **_kwargs):
                attempts.append(port)
                self.port = port
                self.sock = self
            def connect(self):
                if self.port == HTTPS_PORT:
                    raise ConnectionRefusedError('default port unavailable')
            def getpeercert(self, binary_form=False):
                assert binary_form
                return b'peer-cert'
            def request(self, method, url):
                requested.append((self.port, method, url))
            def getresponse(self):
                return type('Response', (), {
                    'status': 200,
                    'read': lambda _self, _limit: b'<root />'})()
            def close(self):
                pass
        with mock.patch.object(host, '_context', return_value=object()), \
             mock.patch.object(host, '_get_public_serverinfo', return_value=(
                 b'<root status_code="200"><HttpsPort>50000</HttpsPort></root>'
             )) as discovery, \
             mock.patch('resources.lib.util.hostapi.spki_sha256',
                        return_value=b'A' * 32), \
             mock.patch('resources.lib.util.hostapi.httplib.HTTPSConnection',
                        side_effect=Connection):
            self.assertEqual(host._get('/applist', []), b'<root />')
        self.assertEqual(attempts, [HTTPS_PORT, 50000])
        self.assertEqual(len(requested), 1)
        self.assertEqual(requested[0][0], 50000)
        self.assertEqual(host.port, 50000)
        discovery.assert_called_once()

    def test_spki_mismatch_never_attempts_port_discovery(self):
        from unittest import mock
        host = Host('host-A')
        host._spki_pin = b'A' * 32
        class Connection:
            sock = type('Socket', (), {
                'getpeercert': lambda self, binary_form=False: b'wrong-key'})()
            def connect(self):
                pass
            def request(self, *_args):
                raise AssertionError('request sent before identity check')
            def close(self):
                pass
        with mock.patch.object(host, '_context', return_value=object()), \
             mock.patch.object(host, '_advertised_https_port') as advertised, \
             mock.patch('resources.lib.util.hostapi.spki_sha256',
                        return_value=b'B' * 32), \
             mock.patch('resources.lib.util.hostapi.httplib.HTTPSConnection',
                        return_value=Connection()):
            with self.assertRaises(HostIdentityError):
                host._get('/applist', [])
        advertised.assert_not_called()


class ApplicationXmlTests(unittest.TestCase):
    def test_escaped_names_and_unicode_are_decoded(self):
        data = (b'<root status_code="200"><App><ID>42</ID>'
                b'<AppTitle>Ratchet &amp; Clank</AppTitle></App>'
                b'<App><ID>99</ID><AppTitle>Pok&#233;mon</AppTitle></App></root>')
        self.assertEqual(Host.parse_applist(data),
                         [('42', 'Ratchet & Clank'), ('99', 'Pok\u00e9mon')])

    def test_empty_app_list_allowed_for_paired_host(self):
        self.assertEqual(Host.parse_applist(b'<root status_code="200"/>'), [])

    def test_explicit_unpaired_and_server_error_rejected(self):
        with self.assertRaisesRegex(HostError, 'not paired'):
            Host.parse_applist(b'<root><PairStatus>0</PairStatus></root>')
        with self.assertRaisesRegex(HostError, 'status'):
            Host.parse_applist(b'<root status_code="503"/>')

    def test_malformed_xml_and_bad_id_rejected(self):
        with self.assertRaisesRegex(HostError, 'malformed'):
            Host.parse_applist(b'<root><App>')
        with self.assertRaisesRegex(HostError, 'incomplete'):
            Host.parse_applist(b'<root><App><ID>../path</ID>'
                               b'<AppTitle>Bad</AppTitle></App></root>')


if __name__ == '__main__':
    unittest.main()
