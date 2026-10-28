"""Small host-side tests for the CoreELEC NG/NO executable selection.

Run with: python3 -m unittest discover -s tests
"""
import os
import stat
import struct
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

sys.modules.setdefault('xbmcaddon', types.ModuleType('xbmcaddon'))
sys.modules.setdefault('xbmc', types.ModuleType('xbmc'))
sys.modules.setdefault('xbmcgui', types.ModuleType('xbmcgui'))

from resources.lib.util.confighelper import ConfigHelper
from resources.lib.util.moonlighthelper import moonlight_environment


def write_elf(path, elf_class):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes((127,)) + b'ELF' + bytes((elf_class,)) + b'\0' * 16)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class CoreELECBinaryTests(unittest.TestCase):
    def test_addon_root_path_is_normalized_for_launch_scripts(self):
        # Some Kodi installations return the addon root WITHOUT '/'.
        # ConfigHelper and MoonlightHelper concatenate paths below it.
        for trailing in ('', os.sep):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory) / 'script.luna'
                root.mkdir()
                addon = types.SimpleNamespace(
                    getAddonInfo=lambda key: str(root) + trailing)
                settings = types.SimpleNamespace(
                    getSetting=lambda key: {
                        'host_addr': '127.0.0.1',
                        'launchscript_conf': 'coreelec',
                        'resolution': '1920x1080',
                    }.get(key, ''),
                    getSettingInt=lambda key: 12,
                )
                with mock.patch('xbmcaddon.Addon', return_value=addon, create=True), \
                     mock.patch.object(
                         ConfigHelper, '_find_binary',
                         return_value=str(root / 'bin' / 'coreelec-ng') + os.sep):
                    helper = ConfigHelper(settings, types.SimpleNamespace(info=lambda *_: None))
                expected = str(root) + os.sep
                self.assertEqual(helper.addon_path, expected)
                self.assertEqual(
                    helper.launchscripts_path,
                    str(root / 'resources' / 'launchscripts' / 'coreelec') + os.sep)
                self.assertEqual(
                    helper.addon_path + 'resources/statics/loading.mp4',
                    str(root / 'resources' / 'statics' / 'loading.mp4'))

    def test_addon_managed_config_uses_binary_dir_even_when_etc_writable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'script.luna'
            root.mkdir()
            bin_dir = root / 'bin' / 'coreelec-ng'
            bin_dir.mkdir(parents=True)
            addon = types.SimpleNamespace(
                getAddonInfo=lambda key: str(root))
            settings = types.SimpleNamespace(
                getSetting=lambda key: {
                    'launchscript_conf': 'coreelec',
                    'host_addr': '192.0.2.1',
                    'resolution': '1920x1080'
                }.get(key, ''),
                getSettingInt=lambda key: 12)
            real_access = os.access
            def access(path, mode):
                return True if path == '/etc' else real_access(path, mode)
            with mock.patch('xbmcaddon.Addon', return_value=addon, create=True), \
                 mock.patch.object(ConfigHelper, '_find_binary',
                                   return_value=str(bin_dir) + os.sep), \
                 mock.patch('os.access', side_effect=access):
                helper = ConfigHelper(
                    settings, types.SimpleNamespace(info=lambda *_: None))
            self.assertEqual(
                helper.config_path, str(bin_dir / 'moonlight.conf'))

    def test_ng_selects_arm32_over_incompatible_no_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_elf(root / 'bin' / 'moonlight', 2)
            write_elf(root / 'bin' / 'coreelec-ng' / 'moonlight', 1)
            with mock.patch('struct.calcsize', return_value=4):
                selected = ConfigHelper._find_binary(str(root))
            self.assertEqual(selected, str(root / 'bin' / 'coreelec-ng') + '/')

    def test_ng_rejects_no_only_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_elf(root / 'bin' / 'moonlight', 2)
            # Assume no system-installed Moonlight (our test must be portable).
            original_isfile = os.path.isfile
            with mock.patch('struct.calcsize', return_value=4), \
                 mock.patch('os.path.isfile', side_effect=lambda path:
                            original_isfile(path) and str(path).startswith(str(root))):
                selected = ConfigHelper._find_binary(str(root))
            self.assertIsNone(selected)

    def test_no_selects_arm64(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_elf(root / 'bin' / 'coreelec-no' / 'moonlight', 2)
            with mock.patch('struct.calcsize', return_value=8):
                selected = ConfigHelper._find_binary(str(root))
            self.assertEqual(selected, str(root / 'bin' / 'coreelec-no') + '/')

    def test_external_binary_never_uses_bundled_libraries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'lib').mkdir()
            config = types.SimpleNamespace(addon_path=str(root),
                                           binary_path='/usr/bin/')
            env = moonlight_environment(config)
            self.assertEqual(env.get('LD_LIBRARY_PATH'),
                             os.environ.get('LD_LIBRARY_PATH'))


if __name__ == '__main__':
    unittest.main()
