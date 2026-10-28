#!/usr/bin/env python3
"""Package pinned Luna with the native artifact, complete VFM runtime and licenses."""
import argparse
import importlib.util
import json
from pathlib import Path
import zipfile


def package(export, native, runtime, output):
    here = Path(__file__).resolve().parent
    pins = json.loads((here / 'sources.json').read_text())
    lock = json.loads((export / 'source-lock.json').read_text())
    if lock['sources']['luna']['revision'] != pins['Luna']:
        raise ValueError('Unexpected Luna source revision')
    spec = importlib.util.spec_from_file_location('snapshot_verify', export / 'verify-snapshot.py')
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    verifier.verify(export)
    spec = importlib.util.spec_from_file_location('luna_package', export / 'luna/packaging/package_ng.py')
    packager = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packager)
    packager.package(export / 'luna', native, output, vfm_directory=runtime)
    # The existing Luna packager deliberately includes only manifest runtime
    # objects. Add redistribution notices outside that restricted namespace.
    licenses = sorted((runtime / 'licenses').glob('*.txt'))
    if len(licenses) != 3:
        raise ValueError('Incomplete VFM license set')
    with zipfile.ZipFile(output, 'a', compression=zipfile.ZIP_DEFLATED) as archive:
        for path in licenses:
            packager.zip_entry(archive, 'resources/legal/vfm/' + path.name, path.read_bytes())
        packager.zip_entry(archive, 'resources/legal/vfm/sources.json',
                           (runtime / 'manifest.json').read_bytes())
    with zipfile.ZipFile(output) as archive:
        if len(archive.namelist()) != len(set(archive.namelist())) or archive.testzip():
            raise ValueError('Invalid or duplicate entries in completed addon')
        manifest = json.loads((runtime / 'manifest.json').read_text())
        for name in manifest['objects']:
            if archive.read('script.luna/vfm/' + name) != (runtime / name).read_bytes():
                raise ValueError('Missing or changed VFM payload: ' + name)
    print('Complete installable Luna VFM addon: ' + str(output))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('luna_export', type=Path)
    parser.add_argument('native', type=Path)
    parser.add_argument('runtime', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    package(args.luna_export.resolve(), args.native.resolve(),
            args.runtime.resolve(), args.output.resolve())
