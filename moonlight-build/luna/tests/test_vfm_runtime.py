import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
from xml.etree import ElementTree

sys.modules.setdefault('xbmc', types.ModuleType('xbmc'))
sys.modules.setdefault('xbmcgui', types.ModuleType('xbmcgui'))
from resources.lib.util.moonlighthelper import MoonlightHelper
from resources.lib.util.vfmruntime import verify_bundle, check_runtime, load_modules, check_decoder_device, check_loaded_bridge, VfmRuntimeError


def fixture(root, release='4.9.test'):
    files = {}
    for name in ('lib/libmoonlight-aml-vfm.so', 'lib/libavcodec.so.62',
                 'lib/libavutil.so.60', 'modules/%s/moonlight_vfm.ko' % release):
        module = name.endswith('.ko')
        data = bytearray(64)
        data[:6] = b'\x7fELF' + bytes([2 if module else 1, 1])
        struct.pack_into('<HH', data, 16, 1 if module else 3, 183 if module else 40)
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        files[name] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    manifest = {'schema': 'luna-ng-vfm-v1', 'kernel_release': release,
                'module_order': ['moonlight_vfm.ko'], 'objects': files}
    (root / 'manifest.json').write_text(json.dumps(manifest))
    return manifest


class VfmRuntimeTests(unittest.TestCase):
    def test_game_mode_setting_is_forwarded_for_both_pipelines(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.config_helper = types.SimpleNamespace(binary_path='/addon/bin/coreelec-ng/', addon_path='/addon')
        for pipeline in ('aml', 'aml-vfm'):
            for enabled in (False, True):
                helper.plugin = types.SimpleNamespace(
                    getSettingBool=lambda name: enabled if name == 'tv_game_mode' else False,
                    getSettingString=lambda name: pipeline if name == 'video_pipeline' else '')
                with mock.patch('resources.lib.util.moonlighthelper.moonlight_environment', return_value={}):
                    env = helper._stream_environment()
                self.assertEqual(env['MOONLIGHT_TV_GAME_MODE'], '1' if enabled else '0')

    def test_decoder_device_must_exist_after_loading_modules(self):
        with tempfile.TemporaryDirectory() as tmp:
            mode = Path(tmp) / 'module/amvdec_ports/parameters/multiplanar'
            mode.parent.mkdir(parents=True)
            mode.write_text('N\n')
            with self.assertRaisesRegex(VfmRuntimeError, 'did not appear'):
                check_decoder_device(tmp, '/unused')
            self.assertEqual(mode.read_text().strip(), '1')

    def test_device_probe_checks_caps_and_formats_and_closes_fd(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            mode = root / 'module/amvdec_ports/parameters/multiplanar'
            mode.parent.mkdir(parents=True)
            mode.write_text('Y\n')
            name = root / 'class/video4linux/video26/name'
            name.parent.mkdir(parents=True)
            name.write_text('aml-vcodec-dec\n')
            for missing in (False, True):
                def ioctl(fd, command, buffer):
                    self.assertEqual(fd, 123)
                    if command == 0x80685600:
                        struct.pack_into('=II', buffer, 84, 0x80000000, 0x04004000)
                    else:
                        index, queue = struct.unpack_from('=II', buffer)
                        formats = [b'H264', b'HEVC'] if queue == 10 else ([b'NV21'] if missing else [b'NV12'])
                        if index >= len(formats):
                            raise OSError(22, 'end of formats')
                        buffer[44:48] = formats[index]
                with mock.patch('resources.lib.util.vfmruntime.os.open', return_value=123), \
                     mock.patch('resources.lib.util.vfmruntime.os.close') as close, \
                     mock.patch('resources.lib.util.vfmruntime.fcntl.ioctl', side_effect=ioctl):
                    if missing:
                        with self.assertRaisesRegex(VfmRuntimeError, 'capture formats'):
                            check_decoder_device(tmp, '/dev')
                    else:
                        check_decoder_device(tmp, '/dev')
                    close.assert_called_once_with(123)

    def test_loaded_old_bridge_requires_reboot_without_unloading_drivers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture(root, os.uname().release)
            exists = Path.exists
            with mock.patch.object(Path, 'exists', lambda path: True if str(path) == '/sys/module/moonlight_vfm' else exists(path)), \
                 mock.patch('resources.lib.util.vfmruntime.subprocess.run') as run:
                with self.assertRaisesRegex(VfmRuntimeError, 'Reboot CoreELEC once'):
                    load_modules(root)
                run.assert_not_called()

    def test_setting_defaults_to_existing_backend(self):
        root = ElementTree.parse(Path(__file__).resolve().parents[1] / 'resources/settings.xml')
        setting = next(s for s in root.iter('setting') if s.get('id') == 'video_pipeline')
        self.assertEqual(setting.findtext('default'), 'aml')
        self.assertEqual([o.text for o in setting.findall('constraints/options/option')],
                         ['aml', 'aml-vfm'])

    def test_loaded_bridge_requires_dma_fix_version(self):
        with mock.patch.object(Path, 'exists', return_value=True), \
             mock.patch.object(Path, 'is_file', return_value=True):
            for version in ('1', '2', '', 'unknown'):
                with self.subTest(version=version), \
                     mock.patch.object(Path, 'read_text', return_value=version):
                    with self.assertRaisesRegex(VfmRuntimeError, 'Reboot CoreELEC once'):
                        check_loaded_bridge()
            with mock.patch.object(Path, 'read_text', return_value='3\n'):
                check_loaded_bridge()

    def test_runtime_selection_only_adds_private_ffmpeg_when_selected(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.config_helper = types.SimpleNamespace(binary_path='/addon/bin/coreelec-ng/',
                                                     addon_path='/addon')
        for selection in ('', 'aml', 'invalid', 'aml-vfm'):
            helper.plugin = types.SimpleNamespace(
                getSettingBool=lambda name: False,
                getSettingString=lambda name: selection if name == 'video_pipeline' else '')
            with mock.patch('resources.lib.util.moonlighthelper.moonlight_environment',
                            return_value={'LD_LIBRARY_PATH': '/addon/lib/coreelec-ng',
                                          'MOONLIGHT_VFM_ROOT': 'stale'}):
                env = helper._stream_environment()
            if selection == 'aml-vfm':
                self.assertEqual(env['MOONLIGHT_VFM_ROOT'], '/addon/vfm')
                self.assertEqual(env['LD_LIBRARY_PATH'], '/addon/vfm/lib:/addon/lib/coreelec-ng')
            else:
                self.assertEqual(env['MOONLIGHT_VIDEO_PIPELINE'], 'aml')
                self.assertNotIn('MOONLIGHT_VFM_ROOT', env)
                self.assertEqual(env['LD_LIBRARY_PATH'], '/addon/lib/coreelec-ng')

    def test_arm32_libraries_with_arm64_kernel_module_are_valid(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture(root)
            self.assertEqual(verify_bundle(root, '4.9.test')['schema'], 'luna-ng-vfm-v1')
            with self.assertRaises(VfmRuntimeError):
                verify_bundle(root, '5.15.test')

    def test_hash_failure_prevents_loader_or_module_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'vfm'
            fixture(root, os.uname().release)
            (root / 'lib/libavcodec.so.62').write_bytes(b'corrupt')
            with mock.patch('resources.lib.util.vfmruntime.subprocess.run') as run:
                with self.assertRaises(VfmRuntimeError):
                    check_runtime(tmp, '/addon/bin/coreelec-ng/moonlight', {})
                with self.assertRaises(VfmRuntimeError):
                    load_modules(root)
                run.assert_not_called()

    def test_wrong_library_abi_rejected_even_with_matching_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = fixture(root)
            name = 'lib/libavcodec.so.62'
            data = bytearray((root / name).read_bytes())
            data[4] = 2
            struct.pack_into('<H', data, 18, 183)
            (root / name).write_bytes(data)
            manifest['objects'][name]['sha256'] = hashlib.sha256(data).hexdigest()
            (root / 'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(VfmRuntimeError, 'Wrong ELF'):
                verify_bundle(root)

    def test_userspace_preflight_is_read_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture(Path(tmp) / 'vfm', os.uname().release)
            result = subprocess.CompletedProcess([], 0, 'AML-VFM userspace dependencies ready')
            with mock.patch('resources.lib.util.vfmruntime.subprocess.run', return_value=result) as run:
                check_runtime(tmp, '/addon/bin/coreelec-ng/moonlight', {'A': 'B'})
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.args[0], ['/addon/bin/coreelec-ng/moonlight', 'check-vfm'])

    def test_missing_backend_blocks_preflight(self):
        with tempfile.TemporaryDirectory() as tmp:
            fixture(Path(tmp) / 'vfm', os.uname().release)
            result = subprocess.CompletedProcess([], 1, 'backend unavailable')
            with mock.patch('resources.lib.util.vfmruntime.subprocess.run', return_value=result):
                with self.assertRaises(VfmRuntimeError):
                    check_runtime(tmp, '/addon/bin/coreelec-ng/moonlight', {})

    def test_optional_bundle_is_packaged_separately(self):
        from test_package_ng import package_ng, write_bundle
        import zipfile
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            root = base / 'vfm'
            fixture(root)
            standard = base / 'standard.tar.gz'
            write_bundle(standard)
            output = base / 'addon.zip'
            package_ng.package(Path(__file__).resolve().parents[1], standard, output,
                               vfm_directory=root)
            with zipfile.ZipFile(output) as archive:
                self.assertIn('script.luna/vfm/lib/libavcodec.so.62', archive.namelist())
                self.assertIn('script.luna/vfm/modules/4.9.test/moonlight_vfm.ko', archive.namelist())
                self.assertIn('script.luna/lib/coreelec-ng/libmoonlight-aml.so', archive.namelist())
                self.assertEqual(archive.read('script.luna/vfm/manifest.json'),
                                 (root / 'manifest.json').read_bytes())

    def test_failed_preflight_keeps_kodi_and_player_running(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.config_helper = types.SimpleNamespace(
            binary_path='/addon/bin/coreelec-ng/', addon_path='/addon',
            launchscripts_path='/addon/resources/launchscripts/coreelec/',
            configure=lambda: None)
        helper.plugin = types.SimpleNamespace(getSettingBool=lambda name: False)
        with mock.patch('resources.lib.util.runtimecheck.check_ng_runtime'), \
             mock.patch('resources.lib.util.moonlighthelper.moonlight_environment', return_value={}), \
             mock.patch.object(helper, '_stream_environment', return_value={'MOONLIGHT_VIDEO_PIPELINE': 'aml-vfm'}), \
             mock.patch('resources.lib.util.vfmruntime.check_runtime', side_effect=VfmRuntimeError('missing')), \
             mock.patch('resources.lib.util.moonlighthelper.xbmcgui.Dialog', create=True) as dialog, \
             mock.patch('resources.lib.util.moonlighthelper.xbmc.Player', create=True) as player, \
             mock.patch.object(helper, '_launch_coreelec_ng_service') as launch:
            helper.launch_game('Desktop')
            dialog.return_value.ok.assert_called_once()
            player.assert_not_called()
            launch.assert_not_called()

    def test_paths_and_load_order_cannot_escape_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = fixture(root)
            manifest['module_order'] = ['../../outside.ko', 'moonlight_vfm.ko']
            (root / 'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(VfmRuntimeError):
                verify_bundle(root)


if __name__ == '__main__':
    unittest.main()
