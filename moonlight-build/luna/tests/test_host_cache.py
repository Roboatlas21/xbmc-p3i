"""Host switches must never reuse another Sunshine server's apps or artwork."""
import json
import os
import threading
import sys
import tempfile
import types
import struct
import zlib
import unittest
from pathlib import Path
from unittest import mock

sys.modules.setdefault('xbmc', types.ModuleType('xbmc'))
sys.modules.setdefault('xbmcgui', types.ModuleType('xbmcgui'))
sys.modules.setdefault('xbmcvfs', types.ModuleType('xbmcvfs'))

from resources.lib.util.moonlighthelper import MoonlightHelper



def sample_png(seed):
    # One true PNG pixel: valid IHDR, zlib scanline, chunk CRCs and IEND.
    rgb = bytes(((sum(seed.encode('utf-8')) + i) % 256 for i in range(3)))
    def chunk(kind, payload):
        crc = zlib.crc32(kind + payload) & 0xffffffff
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', crc)
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(b'\x00' + rgb)) +
            chunk(b'IEND', b''))


class HostCacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.patch = mock.patch.object(
            sys.modules['xbmcvfs'], 'translatePath',
            return_value=str(self.root), create=True)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.helper = MoonlightHelper.__new__(MoonlightHelper)
        self.helper.config_helper = types.SimpleNamespace(
            config={'host_addr': 'first-host'})

    def test_switching_hosts_invalidates_fresh_applist(self):
        self.helper.list_games_detailed = mock.Mock(
            return_value=[('42', 'Host A Game', None)])
        self.assertEqual(self.helper.cached_games()[0][1], 'Host A Game')
        self.helper.config_helper.config['host_addr'] = 'second-host'
        self.helper.list_games_detailed.return_value = [('42', 'Host B Game', None)]
        self.assertEqual(self.helper.cached_games()[0][1], 'Host B Game')
        self.assertEqual(self.helper.list_games_detailed.call_count, 2)
        self.helper.list_games_detailed.reset_mock()
        self.assertEqual(self.helper.cached_games()[0][1], 'Host B Game')
        self.helper.list_games_detailed.assert_not_called()

    def test_legacy_unscoped_cache_is_never_reused(self):
        (self.root / 'applist.json').write_text('[["42", "Old Game"]]')
        self.helper.list_games_detailed = mock.Mock(
            return_value=[('42', 'Current Game', None)])
        self.assertEqual(self.helper.cached_games()[0][1], 'Current Game')

    def test_cache_refresh_migrates_legacy_owner_before_replacement(self):
        from resources.lib.util.library import Library
        (self.root / 'folders.json').write_text(json.dumps({
            'folders': ['Games'], 'assign': {'42': 'Games'}}))
        (self.root / 'applist.json').write_text(json.dumps({
            'host': Library.host_identity('first-host'), 'apps': []}))
        self.helper.config_helper.config['host_addr'] = 'second-host'
        self.helper.list_games_detailed = mock.Mock(
            return_value=[('42', 'Second Host Game', None)])
        self.assertEqual(self.helper.cached_games()[0][1], 'Second Host Game')
        self.assertEqual(Library(str(self.root), host='first-host').folder_of('42'), 'Games')
        self.assertIsNone(Library(str(self.root), host='second-host').folder_of('42'))

    def test_simultaneous_cache_refreshes_use_distinct_temp_files(self):
        barrier = threading.Barrier(2)
        paths = []
        guard = threading.Lock()
        original_replace = os.replace
        errors = []
        def replacing(src, dest):
            if dest == str(self.root / 'applist.json'):
                with guard:
                    paths.append(src)
                barrier.wait(timeout=5)
            return original_replace(src, dest)
        def worker(name):
            try:
                helper = MoonlightHelper.__new__(MoonlightHelper)
                helper.config_helper = types.SimpleNamespace(
                    config={'host_addr': 'first-host'})
                helper.list_games_detailed = lambda: [('42', name, None)]
                helper.cached_games()
            except Exception as error:
                errors.append(error)
        with mock.patch('os.replace', side_effect=replacing):
            threads = [threading.Thread(target=worker, args=(name,))
                       for name in ('Long Name 1234567890', 'Short')]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=8)
        self.assertTrue(all(not thread.is_alive() for thread in threads))
        self.assertFalse(errors)
        self.assertEqual(len(paths), 2)
        self.assertEqual(len(set(paths)), 2)
        self.assertEqual(json.loads((self.root / 'applist.json').read_text())['host'],
                         self.helper._host_cache_key('first-host'))

    def test_incomplete_cached_artwork_is_replaced_atomically(self):
        address = 'first-host'
        folder = self.root / self.helper._host_cache_key(address)
        folder.mkdir(parents=True)
        cached = folder / '42.png'
        cached.write_bytes(b'\x89PNG\r\n\x1a\ntruncated')
        self.assertIsNone(self.helper._art_if_present(address, '42'))

        class StubHost:
            def __init__(self, address):
                pass
            def boxart(self, app_id):
                return sample_png('first-host')

        with mock.patch('resources.lib.util.hostapi.Host', StubHost):
            self.assertEqual(self.helper.boxart_path(address, '42'), str(cached))
        self.assertEqual(cached.read_bytes(), sample_png('first-host'))
        self.assertEqual(list(folder.glob('.boxart.*')), [])

    def test_failed_artwork_replacement_never_exposes_partial_file(self):
        address = 'first-host'
        class StubHost:
            def __init__(self, address):
                pass
            def boxart(self, app_id):
                return sample_png('first-host')
        with mock.patch('resources.lib.util.hostapi.Host', StubHost), \
             mock.patch('resources.lib.util.moonlighthelper.os.replace',
                        side_effect=OSError('disk failure')):
            self.assertIsNone(self.helper.boxart_path(address, '42'))
        folder = self.root / self.helper._host_cache_key(address)
        self.assertEqual(list(folder.iterdir()), [])


    def test_jpeg_without_decoder_has_short_cache_lifetime(self):
        from resources.lib.util import artwork
        address = 'first-host'
        jpeg = (b'\xff\xd8\xff\xfe\x00\x09comment' +
                b'\xff\xc0\x00\x08\x08\x00\x01\x00\x01\x01' +
                b'\xff\xda\x00\x06\x01\x01\x00\x00' +
                b'\x11\x22\x33\x44\xff\xd9')

        class StubHost:
            requests = 0
            def __init__(self, address):
                pass
            def boxart(self, app_id):
                self.__class__.requests += 1
                return jpeg

        with mock.patch('resources.lib.util.hostapi.Host', StubHost), \\
             mock.patch.object(artwork, '_decode_jpeg_if_available',
                               return_value=None):
            first = self.helper.boxart_path(address, '42')
            self.assertIsNotNone(first)
            self.assertEqual(self.helper._art_if_present(address, '42'), first)
            self.assertEqual(self.helper.boxart_path(address, '42'), first)
            self.assertEqual(StubHost.requests, 1)
            os.utime(first, (1, 1))  # Expire an unverified JPEG.
            self.assertIsNone(self.helper._art_if_present(address, '42'))
            self.assertEqual(self.helper.boxart_path(address, '42'), first)
            self.assertEqual(StubHost.requests, 2)

    def test_artwork_and_invalid_ids_isolated(self):
        class StubHost:
            def __init__(self, address):
                self.address = address
            def boxart(self, app_id):
                return sample_png(self.address)
        with mock.patch('resources.lib.util.hostapi.Host', StubHost):
            first = self.helper.boxart_path('first-host', '42')
            second = self.helper.boxart_path('second-host', '42')
        self.assertNotEqual(first, second)
        self.assertEqual(Path(first).read_bytes(), sample_png('first-host'))
        self.assertEqual(Path(second).read_bytes(), sample_png('second-host'))
        self.assertEqual(self.helper._art_if_present('first-host', '42'), first)
        self.assertEqual(self.helper._art_if_present('second-host', '42'), second)
        self.assertIsNone(self.helper.boxart_path('first-host', '../42'))


if __name__ == '__main__':
    unittest.main()
