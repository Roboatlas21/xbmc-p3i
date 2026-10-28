import tempfile
import unittest
from pathlib import Path
from xml.etree import ElementTree

from resources.lib.util.library import Library


class ImportedVortexTests(unittest.TestCase):
    def test_folder_assignment_survives_reload(self):
        with tempfile.TemporaryDirectory() as directory:
            lib = Library(directory)
            self.assertTrue(lib.add_folder('Desktop'))
            self.assertTrue(lib.assign(101, 'Desktop'))
            self.assertFalse(lib.add_folder('Desktop'))
            lib = Library(directory)
            folders, loose = lib.split([('101', 'Desktop', None), ('202', 'Other', None)])
            self.assertEqual(folders, [('Desktop', [('101', 'Desktop', None)])])
            self.assertEqual(loose, [('202', 'Other', None)])

    def test_folder_rename_and_delete_leave_apps_visible(self):
        with tempfile.TemporaryDirectory() as directory:
            lib = Library(directory)
            lib.add_folder('Apps')
            lib.assign('42', 'Apps')
            self.assertTrue(lib.rename_folder('Apps', 'Utilities'))
            self.assertEqual(lib.folder_of('42'), 'Utilities')
            self.assertTrue(lib.remove_folder('Utilities'))
            self.assertIsNone(lib.folder_of('42'))

    def test_imported_settings_include_device_and_overlay_controls(self):
        root = Path(__file__).resolve().parents[1]
        settings = ElementTree.parse(root / 'resources/settings.xml')
        ids = {node.get('id') for node in settings.iter('setting')}
        for required in ('host_addr', 'framerate', 'input_devices',
                         'audio_device_parameter', 'osd',
                         'pair_status_display', 'paired_state',
                         'repair_host', 'unpair_host'):
            self.assertIn(required, ids)
        self.assertTrue((root / 'resources/lib/util/hostapi.py').is_file())
        self.assertTrue((root / 'resources/icons/apps.png').is_file())

    def test_startup_intro_is_off_by_default(self):
        root = Path(__file__).resolve().parents[1]
        settings = ElementTree.parse(root / 'resources/settings.xml')
        matching = [
            node for node in settings.iter('setting')
            if node.get('id') == 'show_intro'
        ]
        self.assertEqual(len(matching), 1)
        self.assertEqual(matching[0].findtext('default'), 'false')


if __name__ == '__main__':
    unittest.main()
