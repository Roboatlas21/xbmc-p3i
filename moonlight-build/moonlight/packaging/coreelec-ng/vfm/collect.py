#!/usr/bin/env python3
"""Collect the optional runtime into a NEW directory for Luna --vfm-directory.

The main ARM32 Moonlight artifact must separately be built with ENABLE_AML_VFM=ON.
No builds, downloads, module loads, or writes to an installed CoreELEC system.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess


def collect(libraries, modules, release, output):
    if not re.fullmatch(r'[A-Za-z0-9._+-]+', release):
        raise ValueError('Invalid kernel release')
    if not modules or modules[-1].name != 'moonlight_vfm.ko':
        raise ValueError('List decoder modules in dependency order, bridge last')
    selected = [libraries / 'libmoonlight-aml-vfm.so']
    for stem in ('libavcodec', 'libavutil'):
        matches = [p for p in libraries.glob(stem + '.so.*') if
                   re.fullmatch(stem + r'\.so\.\d+', p.name)]
        if len(matches) != 1:
            raise ValueError('Expected one major-version SONAME file for ' + stem)
        selected.extend(matches)
    if (libraries / 'libdrm.so.2').exists():
        selected.append(libraries / 'libdrm.so.2')
    content = {}
    for path in selected + modules:
        data = path.read_bytes()
        is_module = path in modules
        expected = (1, 183) if is_module else (3, 40)
        elf_class = 2 if is_module else 1
        if (len(data) < 20 or data[:6] != b'\x7fELF' + bytes([elf_class, 1]) or
                struct.unpack_from('<HH', data, 16) != expected):
            raise ValueError('Wrong ELF architecture/type: ' + str(path))
        if is_module:
            if not re.fullmatch(r'[A-Za-z0-9_]+\.ko', path.name):
                raise ValueError('Invalid module name')
            magic = subprocess.check_output(['modinfo', '-F', 'vermagic', str(path)], text=True)
            if magic.split()[:1] != [release]:
                raise ValueError('Kernel mismatch: ' + str(path))
        name = ('modules/' + release if is_module else 'lib') + '/' + path.name
        if name in content:
            raise ValueError('Duplicate object: ' + name)
        content[name] = data
    output.mkdir(parents=True, exist_ok=False)
    for name, data in content.items():
        dest = output / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data) # Dereference build-time SONAME symlinks.
    manifest = {
        'schema': 'luna-ng-vfm-v1', 'kernel_release': release,
        'module_order': [p.name for p in modules],
        'objects': {n: {'size': len(d), 'sha256': hashlib.sha256(d).hexdigest()}
                    for n, d in content.items()},
    }
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--libraries', type=Path, required=True)
    parser.add_argument('--kernel-release', required=True)
    parser.add_argument('--module', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    collect(args.libraries, args.module, args.kernel_release, args.output)
