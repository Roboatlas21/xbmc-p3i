"""Persist Luna's confirmed last-run app while Kodi is stopped."""
import os
import hashlib
from pathlib import Path
import stat
import sys
import tempfile
import fcntl
import time
from contextlib import contextmanager
from xml.etree import ElementTree


@contextmanager
def _settings_lock(path, timeout=5.0):
    with open(path.with_name(path.name + '.lock'), 'a+b') as lock:
        deadline = time.monotonic() + timeout
        while True:
            try:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Timed out waiting for Luna settings lock')
                time.sleep(0.05)
        try:
            yield
        finally:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def host_identity(host):
    """Match the normalized host key used for folders and application cache."""
    normalized = (host or '').strip().lower()
    return hashlib.sha256(normalized.encode('utf-8')).hexdigest()[:20] if normalized else ''


def set_last_run(settings_path, app, host=None):
    path = Path(settings_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with _settings_lock(path):
        _set_last_run_locked(path, app, host)


def _set_last_run_locked(path, app, host):
    if not isinstance(app, str) or not app:
        raise ValueError('Confirmed app name is empty')

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

    # Store the application and its confirmed host atomically in settings.xml.
    # Old unscoped Resume state is deliberately not trusted after host changes.
    host_nodes = root.findall('setting[@id="last_run_host"]')
    if host_nodes:
        host_node = host_nodes[0]
        for duplicate in host_nodes[1:]:
            root.remove(duplicate)
    else:
        host_node = ElementTree.SubElement(root, 'setting')
        host_node.set('id', 'last_run_host')
        host_node.tail = '\n'
    host_node.attrib.pop('default', None)
    host_node.text = host_identity(host)

    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
                mode='w', encoding='utf-8', dir=path.parent,
                prefix=path.name + '.', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            os.chmod(temporary, mode)
            tree.write(stream, encoding='unicode', xml_declaration=False)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        # Replacement is committed at this point; directory durability may
        # fail independently on filesystems that do not support directory fsync.
        try:
            directory_fd = os.open(str(path.parent), os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0))
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        except OSError as error:
            print('Luna NG: Resume state replaced, but directory fsync failed: %s' %
                  error, file=sys.stderr)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    if len(sys.argv) not in (4, 5) or sys.argv[1] != 'set':
        print('Usage: resumestate.py set SETTINGS_XML APP [HOST]', file=sys.stderr)
        sys.exit(2)
    try:
        set_last_run(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) == 5 else None)
    except (OSError, ValueError, TimeoutError, ElementTree.ParseError) as error:
        print('Luna NG: Resume state: %s' % error, file=sys.stderr)
        sys.exit(1)
