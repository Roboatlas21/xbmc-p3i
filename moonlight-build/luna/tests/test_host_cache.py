"""Host switches must never reuse another Sunshine server's apps or artwork."""
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

sys.modules.setdefault('xbmc', types.ModuleType('xbmc'))
sys.modules.setdefault('xbmcgui', types.ModuleType('xbmcgui'))
sys.modules.setdefault('xbmcvfs', types.ModuleType('xbmcvfs'))

from resources.lib.util.moonlighthelper import MoonlightHelper


class HostCacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.patch = mock.patch.object(
            sys.modules['xbmcvfs'], 'translatePath',
            return_value=str(self.root))
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

    def test_artwork_and_invalid_ids_isolated(self):
        class StubHost:
            def __init__(self, address):
                self.address = address
            def boxart(self, app_id):
                return b'\x89PNG' + self.address.encode()
        with mock.patch('resources.lib.util.hostapi.Host', StubHost):
            first = self.helper.boxart_path('first-host', '42')
            second = self.helper.boxart_path('second-host', '42')
        self.assertNotEqual(first, second)
        self.assertEqual(Path(first).read_bytes(), b'\x89PNGfirst-host')
        self.assertEqual(Path(second).read_bytes(), b'\x89PNGsecond-host')
        self.assertEqual(self.helper._art_if_present('first-host', '42'), first)
        self.assertEqual(self.helper._art_if_present('second-host', '42'), second)
        self.assertIsNone(self.helper.boxart_path('first-host', '../42'))


if __name__ == '__main__':
    unittest.main()
