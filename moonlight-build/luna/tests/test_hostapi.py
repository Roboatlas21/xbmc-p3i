import tempfile
import unittest
from pathlib import Path

from resources.lib.util.hostapi import Host, DEFAULT_UNIQUE_ID


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


if __name__ == '__main__':
    unittest.main()
