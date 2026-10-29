import tempfile
import unittest
from pathlib import Path

from resources.lib.util.hostapi import Host, HostError, DEFAULT_UNIQUE_ID


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

    def test_short_missing_or_non_ascii_id_uses_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, 'uniqueid.dat')
            self.assertEqual(Host('host', key_dir=tmp)._unique_id(),
                             DEFAULT_UNIQUE_ID)
            path.write_bytes(b'too-short')
            self.assertEqual(Host('host', key_dir=tmp)._unique_id(),
                             DEFAULT_UNIQUE_ID)
            path.write_bytes(b'123456789012345\xff')
            self.assertEqual(Host('host', key_dir=tmp)._unique_id(),
                             DEFAULT_UNIQUE_ID)



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
