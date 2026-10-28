import os
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

xbmc = sys.modules.get('xbmc') or types.ModuleType('xbmc')
xbmc.LOGERROR = 4
xbmc.LOGINFO = 1
xbmc.log = lambda *args, **kwargs: None
sys.modules['xbmc'] = xbmc
sys.modules.setdefault('xbmcgui', types.ModuleType('xbmcgui'))
sys.modules.setdefault('xbmcaddon', types.ModuleType('xbmcaddon'))

from resources.lib.util.confighelper import ConfigHelper
from resources.lib.util.moonlighthelper import MoonlightHelper


class SystemdHandoffTests(unittest.TestCase):
    def test_coreelec_ng_detection_is_path_based(self):
        self.assertTrue(MoonlightHelper._is_coreelec_ng(
            '/storage/.kodi/addons/script.luna.ng/bin/coreelec-ng/'))
        self.assertFalse(MoonlightHelper._is_coreelec_ng(
            '/storage/.kodi/addons/script.luna.ng/bin/coreelec-no/'))

    def test_host_preflight_succeeds_before_kodi_handoff(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        completed = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='1. Desktop\n')

        with mock.patch(
                'resources.lib.util.moonlighthelper.subprocess.run',
                return_value=completed) as runner:
            ok, error = helper._preflight_coreelec_ng_host(
                '/addon/bin/coreelec-ng/',
                {'LD_LIBRARY_PATH': '/addon/lib/coreelec-ng'})

        self.assertTrue(ok)
        self.assertIsNone(error)
        self.assertEqual(
            runner.call_args.args[0],
            ['/addon/bin/coreelec-ng/moonlight', 'list'])
        self.assertEqual(
            runner.call_args.kwargs['cwd'],
            '/addon/bin/coreelec-ng/')
        self.assertEqual(runner.call_args.kwargs['timeout'], 12)

    def test_host_preflight_timeout_leaves_handoff_unstarted(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)

        with mock.patch(
                'resources.lib.util.moonlighthelper.subprocess.run',
                side_effect=subprocess.TimeoutExpired(
                    cmd=['moonlight', 'list'], timeout=12)):
            ok, error = helper._preflight_coreelec_ng_host(
                '/addon/bin/coreelec-ng/', {})

        self.assertFalse(ok)
        self.assertIn('did not respond within 12 seconds', error)
        self.assertIn('Kodi was left running', error)

    def test_systemd_handoff_uses_independent_session_and_filtered_environment(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        completed = subprocess.CompletedProcess(args=[], returncode=0, stdout='')

        env = {
            'HOME': '/storage',
            'PATH': '/usr/bin:/bin',
            'LD_LIBRARY_PATH': '/addon/lib/coreelec-ng',
            'MOONLIGHT_BINARY': '/addon/bin/coreelec-ng/moonlight',
            'MOONLIGHT_AML_PROFILE': '1',
            'SECRET_SHOULD_NOT_LEAK': 'nope',
        }

        real_isfile = os.path.isfile
        with mock.patch(
                'resources.lib.util.moonlighthelper.os.path.isfile',
                side_effect=lambda path: False if path == '/usr/bin/systemd-run'
                else real_isfile(path)), \
             mock.patch(
                'resources.lib.util.moonlighthelper.subprocess.run',
                return_value=completed) as runner:
            ok, error = helper._launch_coreelec_ng_service(
                'Desktop', '/addon/resources/launchscripts/coreelec/', env)

        self.assertTrue(ok)
        self.assertIsNone(error)
        command = runner.call_args.args[0]
        self.assertEqual(command[0], 'systemd-run')
        self.assertIn('--unit=luna-ng-session', command)
        self.assertIn('--collect', command)
        self.assertIn('--no-block', command)
        self.assertIn('/usr/bin/env', command)
        self.assertIn('LUNA_SESSION_UNIT=luna-ng-session.service', command)
        self.assertIn('MOONLIGHT_BINARY=/addon/bin/coreelec-ng/moonlight', command)
        self.assertIn('LD_LIBRARY_PATH=/addon/lib/coreelec-ng', command)
        self.assertNotIn('SECRET_SHOULD_NOT_LEAK=nope', command)
        self.assertEqual(
            command[-2:],
            ['/addon/resources/launchscripts/coreelec/launch.sh', 'Desktop'])
        self.assertEqual(runner.call_args.kwargs['timeout'], 10)

    def test_saved_input_names_are_resolved_to_current_event_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / 'moonlight.conf'

            class Plugin:
                def getSetting(self, key):
                    return 'Keyboard|Mouse' if key == 'input_devices' else ''

                def setSettingString(self, *args):
                    pass

            helper = ConfigHelper.__new__(ConfigHelper)
            helper.plugin = Plugin()
            helper.logger = types.SimpleNamespace(info=lambda *_: None)
            helper.config_path = str(config_path)
            helper.config = {
                'host_addr': '10.0.0.59',
                'resolution': '1920x1080',
                'resolution_width': '1920',
                'resolution_height': '1080',
                'codec': 'h264',
                'bitrate': 20,
                'framerate': '60',
                'surround': 'false',
                'audio_device_parameter': '',
                'packetsize': 1392,
                'gamepad_mouse': 'false',
                'cec': 'true',
                'hdr_flag': 'false',
                'remote_optimizations': 'auto',
                'local_audio': 'true',
                'debug_mode': 'false',
                'osd': 'false',
                'osd_font_size': '20',
                'osd_position': 'right',
                'osd_top': '40',
                'osd_interval': '1.5',
            }

            with mock.patch(
                    'resources.lib.util.confighelper.inputdevices.resolve',
                    return_value=['/dev/input/event7', '/dev/input/event8']) as resolve:
                helper._dump_conf()

            resolve.assert_called_once_with(['Keyboard', 'Mouse'])
            text = config_path.read_text(encoding='utf-8')
            self.assertIn('input = /dev/input/event7\n', text)
            self.assertIn('input = /dev/input/event8\n', text)
            self.assertNotIn('input = Keyboard\n', text)
            self.assertNotIn('input = Mouse\n', text)


if __name__ == '__main__':
    unittest.main()
