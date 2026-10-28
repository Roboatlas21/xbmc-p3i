#!/usr/bin/env python3
"""Collect a complete VFM runtime from the pinned CoreELEC install_pkg outputs."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
BASE_LIBS = {'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2',
             'librt.so.1', 'libgcc_s.so.1', 'ld-linux-armhf.so.3'}
ROOT_MODULES = ('amvdec_ports', 'amvdec_mh264', 'amvdec_h265', 'moonlight_vfm')


def package(root, name):
    matches = [p for p in (root / 'install_pkg').glob(name + '-*')
               if (p / '.libreelec-package').is_file() and
               ('INFO_PKG_NAME="%s"' % name) in
               (p / '.libreelec-package').read_text().splitlines()]
    if len(matches) != 1:
        raise ValueError('Expected one installed package for ' + name)
    return matches[0]


def modinfo(path, field):
    return subprocess.check_output(['modinfo', '-F', field, str(path)], text=True).strip()


def module_order(roots, modules, dependencies):
    """Topological closure; missing dependencies and cycles are fatal."""
    result, visiting, done = [], set(), set()
    def visit(name):
        name = name.replace('-', '_')
        if name in done:
            return
        if name in visiting:
            raise ValueError('Module dependency cycle: ' + name)
        if name not in modules:
            raise ValueError('Missing module dependency: ' + name)
        visiting.add(name)
        for dependency in dependencies[name]:
            visit(dependency)
        visiting.remove(name)
        done.add(name)
        result.append(name)
    for root in roots:
        visit(root)
    if result[-1] != 'moonlight_vfm':
        raise ValueError('VFM bridge must be loaded last')
    return result


def dynamic(path):
    result = subprocess.check_output(['readelf', '-dW', str(path)], text=True)
    sonames = re.findall(r'\(SONAME\).*\[([^\]]+)\]', result)
    needed = re.findall(r'\(NEEDED\).*\[([^\]]+)\]', result)
    for paths in re.findall(r'\((?:RPATH|RUNPATH)\).*\[([^\]]*)\]', result):
        if any(p != '$ORIGIN' for p in paths.split(':')):
            raise ValueError('Nonrelocatable library search path: ' + str(path))
    return sonames, needed


def libdrm_notices(source):
    """libdrm 2.4.120 keeps its notices in source headers, not COPYING."""
    for name in ('xf86drm.c', 'xf86drmHash.c', 'xf86drmRandom.c',
                 'xf86drmSL.c', 'xf86drmMode.c'):
        if not (source / name).is_file():
            raise ValueError('Missing libdrm license source: ' + str(source / name))
    paths = sorted([*source.glob('*.c'), *source.glob('*.h'),
                    *(source / 'include/drm').glob('*.h')])
    sections = ['Notices from ' + source.name + ' core sources and DRM headers.']
    for path in paths:
        notices = [comment for comment in re.findall(r'/\*.*?\*/', path.read_text(), re.S)
                   if re.search(r'copyright|permission is hereby granted|SPDX-License-Identifier',
                                comment, re.I)]
        if not notices:
            raise ValueError('Missing libdrm license notice: ' + str(path))
        sections.append('--- ' + str(path.relative_to(source)) + ' ---\n' +
                        '\n\n'.join(notices))
    return '\n\n'.join(sections) + '\n'


def collect(ce, output):
    pins = json.loads((HERE / 'sources.json').read_text())
    for name, source in (
        ('CoreELEC', ce),
        ('linux-amlogic', ce.parent / 'linux-amlogic-local'),
        ('media_modules-aml', ce / 'sources/media_modules-aml/media_modules-aml-local'),
    ):
        revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'],
                                           text=True).strip()
        if revision != pins[name]:
            raise ValueError('Unreviewed source revision: ' + name)
    builds = [p for p in ce.glob('build.CoreELEC-Amlogic-ng.arm-21*') if p.is_dir()]
    if len(builds) != 1:
        raise ValueError('Expected exactly one CoreELEC NG ARM32 build directory')
    build = builds[0]
    native = package(build, 'moonlight-embedded')
    ffmpeg = package(build, 'moonlight-vfm-ffmpeg')
    drm = package(build, 'libdrm')
    # Match the source version to the installed library rather than selecting
    # an arbitrary libdrm build or assuming an upstream COPYING file exists.
    drm_notices = libdrm_notices(build / 'build' / drm.name)
    linux = package(build, 'linux')
    media = package(build, 'media_modules-aml')
    bridge = package(build, 'moonlight-vfm-driver')
    config = linux / '.image/.config'
    symvers = linux / '.image/Module.symvers'
    if not config.is_file() or not symvers.is_file() or not symvers.stat().st_size:
        raise ValueError('Missing exact kernel configuration/symbol versions')
    for flag in ('CONFIG_ARM64=y', 'CONFIG_COMPAT=y', 'CONFIG_MODVERSIONS=y'):
        if flag not in config.read_text().splitlines():
            raise ValueError('Unsupported kernel configuration: ' + flag)
    releases = list((linux / 'usr/lib/kernel-overlays/base/lib/modules').glob('*'))
    if len(releases) != 1:
        raise ValueError('Ambiguous kernel release')
    release = releases[0].name
    modules, dependencies = {}, {}
    for parent in (linux, media, bridge):
        for path in parent.rglob('*.ko'):
            # Linux 4.9 modpost does not emit MODULE_INFO(name). Its stock
            # Kbuild output filename supplies KBUILD_MODNAME in that case.
            name = (modinfo(path, 'name') or path.stem).replace('-', '_')
            if not re.fullmatch(r'[A-Za-z0-9_]+', name) or name in modules:
                raise ValueError('Invalid/duplicate kernel module: ' + str(path))
            modules[name] = path
            dependencies[name] = [n for n in modinfo(path, 'depends').split(',') if n]
    order = module_order(ROOT_MODULES, modules, dependencies)
    spec = importlib.util.spec_from_file_location('vfm_collect', HERE / 'collect.py')
    collector = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(collector)
    with tempfile.TemporaryDirectory(prefix='moonlight-vfm-collect-') as temp:
        stage = Path(temp)
        libraries = stage / 'lib'
        libraries.mkdir()
        inputs = [native / 'usr/lib/libmoonlight-aml-vfm.so', drm / 'usr/lib/libdrm.so.2']
        for stem in ('libavcodec', 'libavutil'):
            matches = [p for p in (ffmpeg / 'usr/lib/moonlight-vfm/lib').glob(stem + '.so.*')
                       if re.fullmatch(stem + r'\.so\.\d+', p.name)]
            if len(matches) != 1:
                raise ValueError('Missing/ambiguous private FFmpeg SONAME: ' + stem)
            inputs += matches
        shipped = {p.name for p in inputs}
        for path in inputs:
            sonames, needed = dynamic(path)
            if sonames != [path.name] or set(needed) - shipped - BASE_LIBS:
                raise ValueError('Missing/unexpected library dependency or SONAME: ' + str(path))
            shutil.copyfile(path, libraries / path.name)
        # Luna uses /sys/module names: normalize filenames such as v4l2-mem2mem.
        selected = []
        for name in order:
            dest = stage / (name + '.ko')
            shutil.copyfile(modules[name], dest)
            selected.append(dest)
        collector.collect(libraries, selected, release, output)
    manifest_path = output / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['kernel_config_sha256'] = hashlib.sha256(config.read_bytes()).hexdigest()
    manifest['kernel_symvers_sha256'] = hashlib.sha256(symvers.read_bytes()).hexdigest()
    manifest['source_revisions'] = pins
    revision = os.environ.get('SOURCE_GIT_SHA', 'unknown')
    if revision != 'unknown' and not re.fullmatch(r'[a-f0-9]{40}', revision):
        raise ValueError('Invalid Moonlight source revision')
    manifest['moonlight_source_git_sha'] = revision
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    licenses = output / 'licenses'
    licenses.mkdir()
    for pkg in (ffmpeg, bridge):
        for path in (pkg / 'usr/share/licenses/moonlight-vfm').glob('*.txt'):
            shutil.copyfile(path, licenses / path.name)
    (licenses / 'libdrm-notices.txt').write_text(drm_notices)
    if len(list(licenses.glob('*.txt'))) != 3:
        raise ValueError('Incomplete runtime licenses')
    print('Complete VFM runtime: kernel %s, %d modules, %d private libraries' %
          (release, len(order), len(shipped)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('coreelec', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    collect(args.coreelec.resolve(), args.output.resolve())
