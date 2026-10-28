"""Session-scoped HDMI ALLM with a durable snapshot for the Kodi watchdog."""
import json
import os
from pathlib import Path
import sys


CONTENT_TYPES = {'off', 'game', 'graphics', 'photo', 'cinema'}


def read(root, name):
    return (root / name).read_text().strip()


def write(root, name, value):
    (root / name).write_text(value + '\n')


def restore(state_file):
    state_file = Path(state_file)
    if not state_file.exists() or not state_file.stat().st_size:
        return
    saved = json.loads(state_file.read_text())
    if (saved.get('schema') != 1 or saved.get('allm') != '0' or
            saved.get('content') not in CONTENT_TYPES):
        raise ValueError('Invalid HDMI state snapshot')
    root = Path(saved['root'])
    current = read(root, 'allm_mode')
    content = read(root, 'contenttype_mode')
    if current not in ('0', '1'):
        raise ValueError('Unexpected HDMI ALLM state during restore')
    if content != saved['content']:
        print('Luna NG: HDMI content mode changed externally; leaving it unchanged')
    else:
        # This CoreELEC setter clears ALLM and restores the AVI content hint
        # in one write. Setting allm_mode=0 alone would leave the content hint
        # cleared by ALLM activation, despite its stale sysfs readback.
        write(root, 'contenttype_mode', saved['content'])
        if read(root, 'allm_mode') != '0' or read(root, 'contenttype_mode') != saved['content']:
            raise OSError('HDMI state restoration was not confirmed')
        print('Luna NG: restored HDMI Game Mode signalling before returning to Kodi')
    state_file.unlink()


def enable(state_file, hdmi_root='/sys/class/amhdmitx/amhdmitx0'):
    state_file = Path(state_file)
    root = Path(hdmi_root)
    if read(root, 'allm_cap') != '1':
        print('Luna NG: connected display does not advertise ALLM; Game Mode skipped')
        return
    original = read(root, 'allm_mode')
    if original == '1':
        print('Luna NG: HDMI ALLM already enabled; preserving the existing setting')
        return
    content = read(root, 'contenttype_mode')
    if original != '0' or content not in CONTENT_TYPES:
        raise ValueError('Unexpected HDMI state; Game Mode skipped')
    saved = {'schema': 1, 'root': str(root), 'allm': original, 'content': content}
    temporary = state_file.with_name(state_file.name + '.tmp')
    try:
        with open(temporary, 'w', encoding='utf-8') as stream:
            os.chmod(temporary, 0o600)
            json.dump(saved, stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, state_file)
    finally:
        if temporary.exists():
            temporary.unlink()
    try:
        write(root, 'allm_mode', '1')
        if read(root, 'allm_mode') != '1':
            raise OSError('HDMI ALLM activation was not confirmed')
    except (OSError, ValueError):
        restore(state_file)
        raise
    print('Luna NG: requested TV Game Mode using HDMI ALLM')


if __name__ == '__main__':
    try:
        if len(sys.argv) == 4 and sys.argv[1] == 'enable':
            enable(sys.argv[2], sys.argv[3])
        elif len(sys.argv) == 3 and sys.argv[1] == 'restore':
            restore(sys.argv[2])
        else:
            raise ValueError('Usage: hdmigamemode.py enable STATE HDMI_ROOT | restore STATE')
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('Luna NG: HDMI Game Mode: %s' % error, file=sys.stderr)
        sys.exit(1)
