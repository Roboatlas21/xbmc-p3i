"""Exercise complete install_pkg collection and reject incomplete runtime bundles."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
VFM = ROOT / 'packaging/coreelec-ng/vfm'
spec = importlib.util.spec_from_file_location('runtime', VFM / 'collect-runtime.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)
PINS = json.loads((VFM / 'sources.json').read_text())


def elf(module=False):
    data = bytearray(64)
    data[:6] = b'\x7fELF' + bytes([2 if module else 1, 1])
    struct.pack_into('<HH', data, 16, *((1, 183) if module else (3, 40)))
    return data


class RuntimeBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ce = self.root / 'CoreELEC'
        self.build = self.ce / 'build.CoreELEC-Amlogic-ng.arm-21.3'
        self.output = self.root / 'vfm'
        self.pkgs = {}
        for name in ('moonlight-embedded', 'moonlight-vfm-ffmpeg', 'libdrm',
                     'linux', 'media_modules-aml', 'moonlight-vfm-driver'):
            path = self.build / 'install_pkg' / (name + '-1')
            path.mkdir(parents=True)
            (path / '.libreelec-package').write_text('INFO_PKG_NAME="%s"\n' % name)
            self.pkgs[name] = path
        self.release = '4.9.269'
        image = self.pkgs['linux'] / '.image'
        image.mkdir()
        (image / '.config').write_text('CONFIG_ARM64=y\nCONFIG_COMPAT=y\nCONFIG_MODVERSIONS=y\n')
        (image / 'Module.symvers').write_text('0x12345678\tvf_reg_provider\tvmlinux\n')
        self.graph = {'videodev': [], 'v4l2_mem2mem': ['videodev'],
                      'decoder_common': [], 'amvdec_ports': ['v4l2-mem2mem', 'decoder_common'],
                      'amvdec_mh264': ['decoder_common'], 'amvdec_h265': ['decoder_common'],
                      'moonlight_vfm': []}
        self.modules = {}
        for name in self.graph:
            pkg = ('moonlight-vfm-driver' if name == 'moonlight_vfm' else
                   'linux' if name in ('videodev', 'v4l2_mem2mem') else 'media_modules-aml')
            path = self.pkgs[pkg] / 'usr/lib/kernel-overlays/base/lib/modules' / self.release
            path.mkdir(parents=True, exist_ok=True)
            # Linux's installed filename contains a hyphen, its module name an underscore.
            path /= name.replace('_', '-') + '.ko'
            path.write_bytes(elf(True))
            self.modules[name] = path
        self.libs = {}
        for name, pkg, directory in (
            ('libmoonlight-aml-vfm.so', 'moonlight-embedded', 'usr/lib'),
            ('libdrm.so.2', 'libdrm', 'usr/lib'),
            ('libavcodec.so.62', 'moonlight-vfm-ffmpeg', 'usr/lib/moonlight-vfm/lib'),
            ('libavutil.so.60', 'moonlight-vfm-ffmpeg', 'usr/lib/moonlight-vfm/lib'),
        ):
            path = self.pkgs[pkg] / directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(elf())
            self.libs[name] = path
        self.needed = {'libmoonlight-aml-vfm.so': ['libavcodec.so.62', 'libavutil.so.60', 'libc.so.6'],
                       'libavcodec.so.62': ['libavutil.so.60', 'libc.so.6', 'libm.so.6'],
                       'libavutil.so.60': ['libdrm.so.2', 'libc.so.6'], 'libdrm.so.2': ['libc.so.6']}
        self.rpath = ''
        for pkg, filename in (('moonlight-vfm-ffmpeg', 'FFmpeg-LGPL-2.1.txt'),
                              ('moonlight-vfm-driver', 'kernel-GPL-2.0.txt')):
            path = self.pkgs[pkg] / 'usr/share/licenses/moonlight-vfm' / filename
            path.parent.mkdir(parents=True)
            path.write_text('license fixture\n')
        path = self.build / 'build/libdrm-2.4.120/COPYING'
        path.parent.mkdir(parents=True)
        path.write_text('MIT fixture\n')
        self.revisions = dict(PINS)
        self.magic = self.release + ' SMP preempt mod_unload modversions aarch64'
        self.module_names = False  # Linux 4.9 lacks the modinfo name field.
        mock = patch.object(runtime.subprocess, 'check_output', side_effect=self.command)
        mock.start()
        self.addCleanup(mock.stop)

    def command(self, args, **kwargs):
        if args[0] == 'git':
            name = Path(args[2]).name
            key = {'CoreELEC': 'CoreELEC', 'linux-amlogic-local': 'linux-amlogic',
                   'media_modules-aml-local': 'media_modules-aml'}[name]
            return self.revisions[key] + '\n'
        if args[0] == 'modinfo':
            name = Path(args[-1]).stem.replace('-', '_')
            return {'name': name if self.module_names else '', 'depends': ','.join(self.graph[name]),
                    'vermagic': self.magic}[args[2]] + '\n'
        if args[0] == 'readelf':
            name = Path(args[-1]).name
            return '(SONAME) Library soname: [%s]\n%s\n%s' % (
                name, '\n'.join('(NEEDED) Shared library: [%s]' % n for n in self.needed[name]),
                '(RUNPATH) Library runpath: [%s]' % self.rpath if self.rpath else '')
        raise AssertionError(args)

    def collect(self):
        runtime.collect(self.ce, self.output)
        return json.loads((self.output / 'manifest.json').read_text())

    def test_complete_bundle_has_transitive_modules_and_private_libraries(self):
        manifest = self.collect()
        self.assertEqual(manifest['schema'], 'luna-ng-vfm-v1')
        self.assertEqual(manifest['kernel_release'], self.release)
        order = manifest['module_order']
        self.assertEqual(order[-1], 'moonlight_vfm.ko')
        self.assertLess(order.index('videodev.ko'), order.index('v4l2_mem2mem.ko'))
        self.assertLess(order.index('v4l2_mem2mem.ko'), order.index('amvdec_ports.ko'))
        self.assertEqual(len(manifest['objects']), len(self.graph) + 4)
        self.assertEqual(manifest['source_revisions'], PINS)
        for name, info in manifest['objects'].items():
            self.assertEqual((self.output / name).stat().st_size, info['size'])
            self.assertEqual(len(info['sha256']), 64)
        self.assertEqual(len(list((self.output / 'licenses').glob('*.txt'))), 3)

    def test_missing_transitive_module_fails(self):
        self.modules['videodev'].unlink()
        with self.assertRaisesRegex(ValueError, 'Missing module dependency: videodev'):
            self.collect()
        self.assertFalse(self.output.exists())

    def test_explicit_module_names_work_too(self):
        self.module_names = True
        self.assertEqual(self.collect()['module_order'][-1], 'moonlight_vfm.ko')

    def test_cycle_fails(self):
        self.graph['videodev'] = ['v4l2_mem2mem']
        with self.assertRaisesRegex(ValueError, 'cycle'):
            self.collect()

    def test_missing_codec_fails(self):
        self.modules['amvdec_h265'].unlink()
        with self.assertRaisesRegex(ValueError, 'Missing module dependency: amvdec_h265'):
            self.collect()

    def test_wrong_kernel_release_fails(self):
        self.magic = '5.15.0 SMP aarch64'
        with self.assertRaisesRegex(ValueError, 'Kernel mismatch'):
            self.collect()

    def test_wrong_module_architecture_fails(self):
        self.modules['moonlight_vfm'].write_bytes(elf())
        with self.assertRaisesRegex(ValueError, 'Wrong ELF'):
            self.collect()

    def test_wrong_userspace_architecture_fails(self):
        self.libs['libavcodec.so.62'].write_bytes(elf(True))
        with self.assertRaisesRegex(ValueError, 'Wrong ELF'):
            self.collect()

    def test_system_ffmpeg_cannot_replace_missing_private_build(self):
        self.libs['libavcodec.so.62'].unlink()
        with self.assertRaisesRegex(ValueError, 'private FFmpeg SONAME'):
            self.collect()

    def test_missing_dynamic_library_fails(self):
        self.needed['libavcodec.so.62'].append('libmissing.so.9')
        with self.assertRaisesRegex(ValueError, 'library dependency'):
            self.collect()

    def test_absolute_and_mixed_runpaths_fail(self):
        for path in ('/build/sysroot/usr/lib', '$ORIGIN:/build/lib'):
            self.rpath = path
            with self.assertRaisesRegex(ValueError, 'Nonrelocatable'):
                self.collect()

    def test_module_versions_are_required(self):
        (self.pkgs['linux'] / '.image/Module.symvers').unlink()
        with self.assertRaisesRegex(ValueError, 'symbol versions'):
            self.collect()

    def test_changed_kernel_source_fails(self):
        self.revisions['linux-amlogic'] = 'f' * 40
        with self.assertRaisesRegex(ValueError, 'Unreviewed source revision'):
            self.collect()

    def test_duplicate_installed_packages_fail(self):
        duplicate = self.build / 'install_pkg/moonlight-vfm-ffmpeg-old'
        duplicate.mkdir()
        (duplicate / '.libreelec-package').write_text('INFO_PKG_NAME="moonlight-vfm-ffmpeg"\n')
        with self.assertRaisesRegex(ValueError, 'Expected one installed package'):
            self.collect()


class RecipeTests(unittest.TestCase):
    def test_ffmpeg_configure_runs_from_coreelec_build_subdirectory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            configure = root / 'configure'
            configure.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > configured.args\n')
            configure.chmod(0o755)
            build = root / '.arm-linux-gnueabihf'
            build.mkdir()
            script = '''set -eu
source "$1/ffmpeg-package.mk"
PKG_BUILD="$2"
TARGET_ARCH=arm TARGET_CPU=cortex-a53 TARGET_PREFIX=arm-linux-gnueabihf-
SYSROOT_PREFIX=/cross/sysroot TOOLCHAIN=/cross CC=cc AR=ar NM=nm HOST_CC=cc
CFLAGS=-O2 LDFLAGS=
configure_target
'''
            subprocess.run(['bash', '-c', script, 'test', str(VFM), temp],
                           cwd=build, check=True)
            args = (build / 'configured.args').read_text().splitlines()
            self.assertIn('--prefix=/usr/lib/moonlight-vfm', args)
            self.assertIn('--enable-decoder=h264_v4l2m2m,hevc_v4l2m2m', args)

    def test_manual_ffmpeg_install_populates_sysroot_and_runtime(self):
        with tempfile.TemporaryDirectory() as temp:
            script = '''set -eu
source "$1/ffmpeg-package.mk"
SYSROOT_PREFIX="$2/sysroot"
INSTALL="$2/install"
PKG_BUILD="$2"
printf 'LGPL fixture\n' > "$PKG_BUILD/COPYING.LGPLv2.1"
make() { printf '%s\\n' "$*" >> "$log"; }
makeinstall_target
'''
            log = Path(temp) / 'calls'
            subprocess.run(['bash', '-c', script, 'test', str(VFM), temp],
                           env={**os.environ, 'log': str(log)}, check=True)
            self.assertEqual(log.read_text().splitlines(),
                             ['DESTDIR=' + temp + '/sysroot install',
                              'DESTDIR=' + temp + '/install install'])
            self.assertEqual((Path(temp) / 'install/usr/share/licenses/moonlight-vfm/FFmpeg-LGPL-2.1.txt').read_text(),
                             'LGPL fixture\n')


if __name__ == '__main__':
    unittest.main()
