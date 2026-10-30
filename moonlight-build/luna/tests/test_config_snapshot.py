"""Ensure shared Moonlight configuration is atomic and per-launch copies are stable."""
import os
import tempfile
import types
import sys
import unittest
from pathlib import Path
from unittest import mock

# Kodi's xbmcaddon module is not installed in the host Python environment.
sys.modules.setdefault('xbmcaddon', types.ModuleType('xbmcaddon'))
from resources.lib.util.confighelper import ConfigHelper


class SnapshotTests(unittest.TestCase):
    def test_snapshot_uses_original_rendered_settings_after_shared_rewrite(self):
        with tempfile.TemporaryDirectory() as td:
            helper = ConfigHelper.__new__(ConfigHelper)
            helper.config_path = str(Path(td) / 'moonlight.conf')
            helper._launch_config_content = (
                '[Moonlight]\naddress = 192.0.2.10\ncodec = h264\n')
            helper._atomic_config_write(helper.config_path,
                                        helper._launch_config_content)
            snapshot = helper.create_launch_snapshot()
            try:
                self.assertEqual(os.stat(snapshot).st_mode & 0o777, 0o600)
                assert Path(snapshot).read_text().find('192.0.2.10') >= 0
                helper._atomic_config_write(helper.config_path,
                    '[Moonlight]\naddress = 192.0.2.99\n')
                self.assertIn('192.0.2.10', Path(snapshot).read_text())
                self.assertIn('192.0.2.99', Path(helper.config_path).read_text())
            finally:
                os.unlink(snapshot)

    def test_failed_config_replace_keeps_original_and_cleans_temporary(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'moonlight.conf'
            path.write_text('[Moonlight]\naddress = old\n')
            with mock.patch('resources.lib.util.confighelper.os.replace',
                            side_effect=OSError('injected replace failure')):
                with self.assertRaises(OSError):
                    ConfigHelper._atomic_config_write(
                        str(path), '[Moonlight]\naddress = new\n')
            self.assertIn('address = old', path.read_text())
            self.assertFalse(list(Path(td).glob('.luna-config-*')))

    def test_snapshot_writer_reports_failed_fsync_and_cleans_partial(self):
        with tempfile.TemporaryDirectory() as td:
            helper = ConfigHelper.__new__(ConfigHelper)
            helper.config_path = str(Path(td) / 'moonlight.conf')
            helper._launch_config_content = '[Moonlight]\naddress = host\n'
            with mock.patch('resources.lib.util.confighelper.os.fsync',
                            side_effect=OSError('fsync failed')):
                with self.assertRaises(OSError):
                    helper.create_launch_snapshot()
            self.assertFalse(list(Path(td).glob('.luna-stream-*')))


if __name__ == '__main__':
    unittest.main()
