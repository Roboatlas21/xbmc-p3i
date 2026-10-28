#!/usr/bin/env python3
"""Verify exported files against their original Git blob identities."""
import hashlib
import json
from pathlib import Path


def ignored(relative):
    if any(part in ('.git', '__pycache__') for part in relative.parts):
        return True
    return len(relative.parts) >= 2 and relative.parts[:2] in (
        ('third_party', 'moonlight-common-c'),
        ('third_party', 'SDL_GameControllerDB'),
    )


def verify(root):
    lock = json.loads((root / 'source-lock.json').read_text())
    if lock['schema'] != 'luna-public-source-snapshot-v1':
        raise ValueError('Unknown source-lock schema')
    for name, source in lock['sources'].items():
        directory = root / name
        actual = {
            p.relative_to(directory).as_posix()
            for p in directory.rglob('*')
            if not ignored(p.relative_to(directory)) and (p.is_file() or p.is_symlink())
        }
        if actual != set(source['files']):
            raise ValueError('Unexpected or missing snapshot files: ' + name)
        for relative, expected in source['files'].items():
            path = directory / relative
            if path.is_symlink():
                raise ValueError('Unexpected source symlink: ' + relative)
            data = path.read_bytes()
            digest = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            if digest != expected['blob_sha']:
                raise ValueError('Changed snapshot source: ' + str(path))
            if bool(path.stat().st_mode & 0o111) != (expected['mode'] == '100755'):
                raise ValueError('Wrong executable mode: ' + str(path))
        print('%s: %d files match %s' % (name, len(actual), source['revision']))


if __name__ == '__main__':
    verify(Path(__file__).resolve().parent)
