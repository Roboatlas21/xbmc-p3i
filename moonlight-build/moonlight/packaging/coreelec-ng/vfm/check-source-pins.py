#!/usr/bin/env python3
"""Reject stale cross-repository packaging revisions before ARM32 work starts."""
import json
import sys
from pathlib import Path


def check(pins, lock):
    if lock.get('schema') != 'luna-public-source-snapshot-v1':
        raise ValueError('Unexpected public snapshot schema')
    if not isinstance(pins, dict) or not isinstance(lock.get('sources'), dict):
        raise ValueError('Missing packaging/source revision objects')
    for name, expected_repo, pin in (
            ('luna', 'Roboatlas21/Luna', 'Luna'),
            ('moonlight', 'Roboatlas21/moonlight-embedded', None)):
        source = lock['sources'].get(name)
        if not isinstance(source, dict) or source.get('repository') != expected_repo:
            raise ValueError('Unexpected snapshot repository: ' + name)
        actual = source.get('revision')
        if not isinstance(actual, str) or len(actual) != 40:
            raise ValueError('Invalid snapshot revision: ' + name)
        if pin and actual != pins.get(pin):
            raise ValueError('%s packaging source mismatch: pinned %s, snapshot %s' %
                             (name, pins.get(pin), actual))
    return lock['sources']['luna']['revision']


def main():
    if len(sys.argv) != 2:
        raise SystemExit('Usage: check-source-pins.py SNAPSHOT_SOURCE_LOCK_JSON')
    pins = json.loads((Path(__file__).resolve().parent / 'sources.json').read_text())
    lock = json.loads(Path(sys.argv[1]).read_text())
    revision = check(pins, lock)
    print('VFM packaging pin matches Luna snapshot ' + revision)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError) as error:
        print('VFM packaging preflight: ' + str(error), file=sys.stderr)
        sys.exit(1)
