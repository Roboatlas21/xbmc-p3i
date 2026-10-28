"""Persist Luna's confirmed last-run app while Kodi is stopped."""
import os
from pathlib import Path
import stat
import sys
from xml.etree import ElementTree


def set_last_run(settings_path, app):
    if not isinstance(app, str) or not app:
        raise ValueError('Confirmed app name is empty')

    path = Path(settings_path)
    path.parent.mkdir(parents=True, exist_ok=True)
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
        root = ElementTree.Element('settings')
        root.set('version', '2')
        root.text = '\n'
        tree = ElementTree.ElementTree(root)

    nodes = root.findall('setting[@id="last_run"]')
    if nodes:
        node = nodes[0]
        for duplicate in nodes[1:]:
            root.remove(duplicate)
    else:
        node = ElementTree.SubElement(root, 'setting')
        node.set('id', 'last_run')
        node.tail = '\n'

    node.attrib.pop('default', None)
    node.text = app

    temporary = path.with_name(path.name + '.tmp')
    try:
        with open(temporary, 'w', encoding='utf-8') as stream:
            os.chmod(temporary, mode)
            tree.write(stream, encoding='unicode', xml_declaration=False)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[1] != 'set':
        print('Usage: resumestate.py set SETTINGS_XML APP', file=sys.stderr)
        sys.exit(2)
    try:
        set_last_run(sys.argv[2], sys.argv[3])
    except (OSError, ValueError, ElementTree.ParseError) as error:
        print('Luna NG: Resume state: %s' % error, file=sys.stderr)
        sys.exit(1)
