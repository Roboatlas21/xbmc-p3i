"""SPKI pinning must accept renewed certificates with the same key."""
import hashlib
import unittest
from unittest import mock

from resources.lib.util.certpin import spki_sha256, CertificatePinError
from resources.lib.util.hostapi import Host, HostIdentityError


def tlv(tag, content):
    length = len(content)
    if length < 128:
        prefix = bytes((length,))
    else:
        count = (length.bit_length() + 7) // 8
        prefix = bytes((0x80 | count,)) + length.to_bytes(count, 'big')
    return bytes((tag,)) + prefix + content


def fake_certificate(serial, public_key):
    spki = tlv(0x30, tlv(0x30, b'') + tlv(0x03, b'\x00' + public_key))
    tbs = tlv(0x30,
              tlv(0xa0, tlv(0x02, b'\x02')) +
              tlv(0x02, bytes((serial,))) +
              tlv(0x30, b'') + tlv(0x30, b'') +
              tlv(0x30, b'') + tlv(0x30, b'') + spki)
    return tlv(0x30, tbs + tlv(0x30, b'') + tlv(0x03, b'\x00' + b'X' * 40))


class CertificatePublicKeyTests(unittest.TestCase):
    def test_renewal_keeps_pin_but_new_key_changes_it(self):
        first = fake_certificate(1, b'K' * 80)
        renewed = fake_certificate(2, b'K' * 80)
        replaced = fake_certificate(3, b'L' * 80)
        self.assertNotEqual(first, renewed)
        self.assertEqual(spki_sha256(first), spki_sha256(renewed))
        self.assertNotEqual(spki_sha256(first), spki_sha256(replaced))

    def test_malformed_asn1_is_rejected(self):
        data = fake_certificate(1, b'K' * 80)
        for broken in (b'', b'not-a-cert' * 10, data[:-1], data + b'\x00'):
            with self.subTest(length=len(broken)):
                with self.assertRaises(CertificatePinError):
                    spki_sha256(broken)

    def test_mismatch_rejected_before_any_http_query(self):
        import ssl
        from unittest import mock
        good = fake_certificate(1, b'K' * 80)
        other = fake_certificate(1, b'L' * 80)
        host = Host('example.test')
        host._spki_pin = spki_sha256(good)

        class Sock:
            def getpeercert(self, binary_form=False):
                self.assert_binary = binary_form
                return other
        class Conn:
            sock = Sock()
            connect = mock.Mock()
            request = mock.Mock()
            close = mock.Mock()
        conn = Conn()
        with mock.patch.object(host, '_context', return_value=object()), \
             mock.patch('resources.lib.util.hostapi.httplib.HTTPSConnection',
                        return_value=conn):
            with self.assertRaises(HostIdentityError):
                host._get('/applist', [])
        conn.connect.assert_called_once()
        conn.request.assert_not_called()
        conn.close.assert_called_once()

        conn.request.reset_mock()
        conn.sock.getpeercert = lambda binary_form=False: fake_certificate(9, b'K' * 80)
        conn.getresponse = lambda: type('Response', (), {'status': 200, 'read': lambda _self, _: b'ok'})()
        with mock.patch.object(host, '_context', return_value=object()), \
             mock.patch('resources.lib.util.hostapi.httplib.HTTPSConnection',
                        return_value=conn):
            self.assertEqual(host._get('/applist', []), b'ok')
        conn.request.assert_called_once()


if __name__ == '__main__':
    unittest.main()
