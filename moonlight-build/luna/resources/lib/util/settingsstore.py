"""Atomic, serialized settings.xml updates shared with Luna Resume state."""
import os
from pathlib import Path
import stat
import sys
import tempfile
from xml.etree import ElementTree

from resources.lib.util.resumestate import _settings_lock


def update_setting(settings_path, setting_id, value):
    """Update one Kodi setting without truncating settings.xml on write errors.

    The stable <settings.xml>.lock inode is shared with the independent native
    Resume-state writer. Neither reader sees a half-written XML document.
    """
    if (not isinstance(setting_id, str) or not setting_id or
            not isinstance(value, str)):
        raise ValueError('Invalid Kodi setting update')
    path = Path(settings_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with _settings_lock(path):
        if path.is_symlink():
            raise ValueError('Refusing symlinked Kodi settings file')
        mode = 0o600
        if path.exists():
            mode = stat.S_IMODE(path.stat().st_mode)
            tree = ElementTree.parse(path)
            root = tree.getroot()
            if root.tag != 'settings':
                raise ValueError('Unexpected Kodi settings root')
        else:
            root = ElementTree.Element('settings', {'version': '2'})
            root.text = '\n'
            tree = ElementTree.ElementTree(root)

        # Do not construct an XPath from setting_id; removing old duplicates
        # also leaves a single authoritative value for Kodi's next read.
        matches = [node for node in root.findall('setting')
                   if node.get('id') == setting_id]
        if matches:
            node = matches[0]
            for duplicate in matches[1:]:
                root.remove(duplicate)
        else:
            node = ElementTree.SubElement(root, 'setting', {'id': setting_id})
            node.tail = '\n'
        node.attrib.pop('default', None)
        node.text = value

        temporary = None
        try:
            with tempfile.NamedTemporaryFile(
                    mode='w', encoding='utf-8', dir=path.parent,
                    prefix=path.name + '.', suffix='.tmp',
                    delete=False) as stream:
                temporary = Path(stream.name)
                os.chmod(temporary, mode)
                tree.write(stream, encoding='unicode', xml_declaration=False)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
            # Keep the same best-effort directory durability behavior as
            # resumestate.py; the replacement is committed at this point.
            try:
                directory_fd = os.open(str(path.parent),
                                       os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0))
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
            except OSError as error:
                print('Luna NG: settings updated but directory fsync failed: %s'
                      % error, file=sys.stderr)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
