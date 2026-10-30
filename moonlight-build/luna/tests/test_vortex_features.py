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

    def test_folders_are_global_but_app_assignments_are_host_scoped(self):
        with tempfile.TemporaryDirectory() as directory:
            first = Library(directory, host='First-Host')
            self.assertTrue(first.add_folder('Gaming'))
            self.assertTrue(first.add_folder('Desktop'))
            self.assertTrue(first.assign('42', 'Gaming'))
            other = Library(directory, host='SECOND-HOST')
            self.assertEqual(other.folders(), ['Gaming', 'Desktop'])
            self.assertIsNone(other.folder_of('42'))
            self.assertTrue(other.assign('42', 'Desktop'))
            self.assertEqual(Library(directory, host='first-host').folder_of('42'), 'Gaming')
            self.assertEqual(Library(directory, host='second-host').folder_of('42'), 'Desktop')
            self.assertTrue(first.rename_folder('Desktop', 'Work'))
            self.assertEqual(Library(directory, host='second-host').folder_of('42'), 'Work')
            self.assertTrue(first.remove_folder('Gaming'))
            self.assertIsNone(Library(directory, host='first-host').folder_of('42'))
            self.assertEqual(Library(directory, host='second-host').folder_of('42'), 'Work')

    def test_legacy_assignments_migrate_only_to_last_cached_host(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'folders.json').write_text(json.dumps({
                'folders': ['Games'], 'assign': {'42': 'Games'}}))
            cached_host = Library.host_identity('old-host')
            (folder / 'applist.json').write_text(json.dumps({'host': cached_host, 'apps': []}))
            other = Library(directory, host='new-host')
            self.assertIsNone(other.folder_of('42'))
            self.assertEqual(other.folders(), ['Games'])
            self.assertTrue(other.assign('43', 'Games'))
            self.assertEqual(Library(directory, host='old-host').folder_of('42'), 'Games')
            self.assertIsNone(Library(directory, host='new-host').folder_of('42'))
            self.assertEqual(Library(directory, host='new-host').folder_of('43'), 'Games')

    def test_legacy_migration_commits_before_cache_is_replaced(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            folders = folder / 'folders.json'
            cache = folder / 'applist.json'
            folders.write_text(json.dumps({
                'folders': ['Games'], 'assign': {'42': 'Games'}}))
            cache.write_text(json.dumps({
                'host': Library.host_identity('old-host'), 'apps': [['42', 'Old']]}))
            self.assertTrue(Library(directory, host='new-host').migrate_legacy())
            # The next app refresh can now replace the old host evidence.
            cache.write_text(json.dumps({
                'host': Library.host_identity('new-host'), 'apps': [['42', 'New']]}))
            self.assertEqual(Library(directory, host='old-host').folder_of('42'), 'Games')
            self.assertIsNone(Library(directory, host='new-host').folder_of('42'))
            self.assertNotIn('assign', json.loads(folders.read_text()))

    def test_legacy_without_cache_is_preserved_for_current_host(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / 'folders.json').write_text(json.dumps({
                'folders': ['Apps'], 'assign': {'17': 'Apps'}}))
            first = Library(directory, host='default-host')
            self.assertEqual(first.folder_of('17'), 'Apps')
            self.assertTrue(first.assign('18', 'Apps'))
            self.assertIsNone(Library(directory, host='other-host').folder_of('17'))

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
