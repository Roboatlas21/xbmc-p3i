import tempfile
import unittest
from pathlib import Path
from xml.etree import ElementTree

from resources.lib.util.resumestate import set_last_run


class ResumeStateTests(unittest.TestCase):
    def test_creates_and_updates_only_last_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'settings.xml'
            set_last_run(path, 'Desktop')
            root = ElementTree.parse(path).getroot()
            self.assertEqual(root.findtext('setting[@id="last_run"]'), 'Desktop')

            other = ElementTree.SubElement(root, 'setting')
            other.set('id', 'debug_mode')
            other.text = 'true'
            ElementTree.ElementTree(root).write(path, encoding='unicode')

            set_last_run(path, 'Steam')
            root = ElementTree.parse(path).getroot()
            self.assertEqual(root.findtext('setting[@id="last_run"]'), 'Steam')
            self.assertEqual(root.findtext('setting[@id="debug_mode"]'), 'true')

    def test_rejects_empty_app_and_symlink(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'settings.xml'
            with self.assertRaises(ValueError):
                set_last_run(path, '')
            target = Path(tmp) / 'target.xml'
            target.write_text('<settings version="2"/>')
            path.symlink_to(target)
            with self.assertRaisesRegex(ValueError, 'symlink'):
                set_last_run(path, 'Desktop')


if __name__ == '__main__':
    unittest.main()
