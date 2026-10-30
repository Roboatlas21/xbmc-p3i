"""Fault and concurrency regression tests for atomic Kodi settings updates."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
from xml.etree import ElementTree

from resources.lib.util.settingsstore import update_setting
from resources.lib.util.resumestate import set_last_run


class AtomicSettingsTests(unittest.TestCase):
    def test_create_and_update_preserves_resume_state(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'settings.xml'
            update_setting(path, 'paired_state', 'true')
            set_last_run(str(path), 'Desktop', '192.0.2.55')
            update_setting(path, 'pair_status_display', 'Paired')
            node = ElementTree.parse(path).getroot()
            self.assertEqual(node.findtext('setting[@id="last_run"]'), 'Desktop')
            self.assertEqual(node.findtext('setting[@id="paired_state"]'), 'true')
            self.assertEqual(node.findtext('setting[@id="pair_status_display"]'),
                             'Paired')
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_replace_failure_preserves_original(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'settings.xml'
            update_setting(path, 'paired_state', 'true')
            original = path.read_bytes()
            with mock.patch('resources.lib.util.settingsstore.os.replace',
                            side_effect=OSError('replace rejected')):
                with self.assertRaises(OSError):
                    update_setting(path, 'paired_state', 'false')
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(list(Path(root).glob('settings.xml.*.tmp')), [])

    def test_symlink_rejected_without_touching_target(self):
        with tempfile.TemporaryDirectory() as root:
            outside = Path(root) / 'outside.xml'
            outside.write_text('<settings/>')
            path = Path(root) / 'settings.xml'
            path.symlink_to(outside)
            with self.assertRaisesRegex(ValueError, 'symlinked'):
                update_setting(path, 'paired_state', 'true')
            self.assertEqual(outside.read_text(), '<settings/>')

    def test_duplicate_setting_id_coalesced(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'settings.xml'
            path.write_text('<settings><setting id="paired_state">false</setting>'
                            '<setting id="paired_state">false</setting></settings>')
            update_setting(path, 'paired_state', 'true')
            nodes = ElementTree.parse(path).getroot().findall(
                'setting[@id="paired_state"]')
            self.assertEqual(len(nodes), 1)
            self.assertEqual(nodes[0].text, 'true')


if __name__ == '__main__':
    unittest.main()
