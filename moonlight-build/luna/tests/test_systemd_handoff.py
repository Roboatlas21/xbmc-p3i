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
from resources.lib.util.moonlighthelper import MoonlightHelper, encode_input_names
from resources.lib.util import inputdevices


class SystemdHandoffTests(unittest.TestCase):
    def test_selected_controller_name_whitelist_survives_delimiters(self):
        encoded = encode_input_names(['Xbox Wireless Controller',
                                      'Bluetooth Pad', 'Name,Pad'])
        self.assertEqual([bytes.fromhex(token).decode('utf-8')
                          for token in encoded.split(',')],
                         ['Xbox Wireless Controller', 'Bluetooth Pad', 'Name,Pad'])
        self.assertEqual(encode_input_names(['Pad', 'Pad']), '506164')
        with self.assertRaises(ValueError):
            encode_input_names([''])
        with self.assertRaises(ValueError):
            encode_input_names(['Name\x00Hidden'])

    def test_stream_environment_sets_only_chosen_names(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        selected = {'input_devices': 'Xbox Wireless Controller|Bluetooth Pad'}
        helper.plugin = types.SimpleNamespace(
            getSetting=lambda key: selected.get(key, ''),
            getSettingBool=lambda key: False,
            getSettingString=lambda key: '')
        helper.config_helper = types.SimpleNamespace(
            binary_path='/addon/bin/coreelec-ng/', addon_path='/addon',
            config={'host_addr':'host'})
        env = helper._stream_environment()
        self.assertEqual(env['MOONLIGHT_INPUT_SELECTORS_V2'],
                         inputdevices.encode_selectors([
                             {'name': 'Xbox Wireless Controller',
                              'kind': 'N', 'value': ''},
                             {'name': 'Bluetooth Pad',
                              'kind': 'N', 'value': ''}]))
        self.assertNotIn('MOONLIGHT_INPUT_NAMES_HEX', env)
        selected['input_devices'] = ''
        self.assertNotIn('MOONLIGHT_INPUT_SELECTORS_V2', helper._stream_environment())

    def test_forget_trusted_server_only_invokes_explicit_local_native_action(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.config_helper = types.SimpleNamespace(
            binary_path='/addon/bin/coreelec-ng/', addon_path='/addon',
            configure=lambda dump=False: None)
        completed = subprocess.CompletedProcess(args=[], returncode=0, stdout='forgot pin')
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                        return_value=completed) as run:
            self.assertTrue(helper.forget_trusted_server('192.168.1.7'))
            self.assertEqual(run.call_args.args[0],
                             ['/addon/bin/coreelec-ng/moonlight',
                              'forget-server-cert', '192.168.1.7'])
        self.assertFalse(helper.forget_trusted_server('-wrong-host'))

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

    def test_host_preflight_uses_exact_session_config(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        complete = subprocess.CompletedProcess(args=[], returncode=0,
                                               stdout='1. Desktop')
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                        return_value=complete) as runner:
            ok, error = helper._preflight_coreelec_ng_host(
                '/addon/bin/coreelec-ng/',
                {'LUNA_STREAM_CONFIG': '/addon/bin/coreelec-ng/.luna-stream-a.conf'})
        self.assertTrue(ok)
        self.assertIsNone(error)
        self.assertEqual(runner.call_args.args[0],
                         ['/addon/bin/coreelec-ng/moonlight', 'list',
                          '-config', '/addon/bin/coreelec-ng/.luna-stream-a.conf'])

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
        units = [arg for arg in command if arg.startswith('--unit=')]
        self.assertEqual(len(units), 1)
        self.assertRegex(units[0], r'^--unit=luna-ng-session-[0-9a-f]{12}$')
        self.assertIn('--collect', command)
        self.assertIn('--no-block', command)
        self.assertIn('/usr/bin/env', command)
        self.assertIn('LUNA_SESSION_UNIT=' + units[0][7:] + '.service', command)
        self.assertIn('MOONLIGHT_BINARY=/addon/bin/coreelec-ng/moonlight', command)
        self.assertIn('LD_LIBRARY_PATH=/addon/lib/coreelec-ng', command)
        self.assertNotIn('SECRET_SHOULD_NOT_LEAK=nope', command)
        self.assertEqual(
            command[-2:],
            ['/addon/resources/launchscripts/coreelec/launch.sh', 'Desktop'])
        self.assertEqual(runner.call_args.kwargs['timeout'], 10)


    def test_session_snapshot_is_forwarded_to_detached_service(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        complete = subprocess.CompletedProcess(args=[], returncode=0, stdout='')
        with tempfile.TemporaryDirectory() as directory:
            snapshot = Path(directory) / '.luna-stream-example.conf'
            snapshot.write_text('[Moonlight]\naddress = host\n')
            env = {'LUNA_STREAM_CONFIG': str(snapshot),
                   'PATH': '/usr/bin:/bin'}
            calls = []

            def run_and_acknowledge(command, **kwargs):
                calls.append(command)
                receipt = next((arg.split('=', 1)[1] for arg in command
                                if arg.startswith('LUNA_LAUNCH_ACK=')), None)
                self.assertIsNotNone(receipt)
                Path(receipt).write_text('armed\n')
                return complete

            with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                            side_effect=run_and_acknowledge):
                ok, error = helper._launch_coreelec_ng_service(
                    'Desktop', '/addon/resources/launchscripts/coreelec/', env)
            self.assertTrue(ok)
            self.assertIsNone(error)
            self.assertIn('LUNA_STREAM_CONFIG=' + str(snapshot), calls[0])
            receipts = list(Path(directory).glob('.luna-ack-*'))
            self.assertEqual(len(receipts), 1)
            self.assertEqual(receipts[0].read_text().strip(), 'observed')
            self.assertIn('LUNA_ACK_REQUIRED=1', calls[0])
            self.assertTrue(snapshot.exists())

    def test_early_rejected_launcher_does_not_claim_handoff_success(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        completed = subprocess.CompletedProcess(args=[], returncode=0, stdout='')
        with tempfile.TemporaryDirectory() as directory:
            snapshot = Path(directory) / '.luna-stream-example.conf'
            snapshot.write_text('[Moonlight]')
            def reject_before_arming(command, **kwargs):
                receipt = next(arg.split('=', 1)[1] for arg in command
                               if arg.startswith('LUNA_LAUNCH_ACK='))
                Path(receipt).write_text('failed\n')
                return completed
            with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                            side_effect=reject_before_arming):
                ok, error = helper._launch_coreelec_ng_service(
                    'Desktop', '/addon/resources/launchscripts/coreelec/',
                    {'LUNA_STREAM_CONFIG': str(snapshot)})
            self.assertFalse(ok)
            self.assertIn('before recovery was armed', error)
            self.assertFalse(list(Path(directory).glob('.luna-ack-*')))

    def test_reconciling_phase_waits_for_armed_receipt(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        completed = subprocess.CompletedProcess(args=[], returncode=0, stdout='')
        with tempfile.TemporaryDirectory() as directory:
            snapshot = Path(directory) / '.luna-stream-example.conf'
            snapshot.write_text('[Moonlight]')
            receipts = []

            def start_recovery(command, **kwargs):
                receipt = next(arg.split('=', 1)[1] for arg in command
                               if arg.startswith('LUNA_LAUNCH_ACK='))
                Path(receipt).write_text('reconciling:1\n')
                receipts.append(receipt)
                return completed

            def finish_recovery(_interval):
                Path(receipts[0]).write_text('armed\n')

            with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                            side_effect=start_recovery), \
                 mock.patch('resources.lib.util.moonlighthelper.time.sleep',
                            side_effect=finish_recovery):
                ok, error = helper._launch_coreelec_ng_service(
                    'Desktop', '/addon/resources/launchscripts/coreelec/',
                    {'LUNA_STREAM_CONFIG': str(snapshot)})
            self.assertTrue(ok)
            self.assertIsNone(error)
            receipts = list(Path(directory).glob('.luna-ack-*'))
            self.assertEqual(len(receipts), 1)
            self.assertEqual(receipts[0].read_text().strip(), 'observed')

    def test_multiple_recoveries_extend_deadline_without_termination(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        complete = subprocess.CompletedProcess(args=[], returncode=0, stdout='')
        with tempfile.TemporaryDirectory() as directory:
            snapshot = Path(directory) / '.luna-stream-example.conf'
            snapshot.write_text('[Moonlight]')
            clock = {'now': 0.0}
            calls = []
            def fake_clock():
                return clock['now']
            def simulate_systemd(command, **kwargs):
                calls.append(command)
                receipt = next(arg.split('=', 1)[1] for arg in command
                               if arg.startswith('LUNA_LAUNCH_ACK='))
                Path(receipt).write_text('reconciling:1\n')
                return complete
            def advance_recovery(_delay):
                clock['now'] += 55
                receipt = list(Path(directory).glob('.luna-ack-*'))[0]
                phase = receipt.read_text().strip()
                receipt.write_text(
                    'reconciling:2\n' if phase == 'reconciling:1'
                    else 'armed\n')
            with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                            side_effect=simulate_systemd), \
                 mock.patch('resources.lib.util.moonlighthelper.time.monotonic',
                            side_effect=fake_clock), \
                 mock.patch('resources.lib.util.moonlighthelper.time.sleep',
                            side_effect=advance_recovery):
                ok, error = helper._launch_coreelec_ng_service(
                    'Desktop', '/addon/resources/launchscripts/coreelec/',
                    {'LUNA_STREAM_CONFIG': str(snapshot)})
            self.assertTrue(ok, error)
            self.assertEqual(len(calls), 1)
            self.assertEqual(list(Path(directory).glob('.luna-ack-*'))[0]
                             .read_text().strip(), 'observed')

    def test_stuck_reconciliation_does_not_kill_existing_recovery(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        complete = subprocess.CompletedProcess(args=[], returncode=0, stdout='')
        with tempfile.TemporaryDirectory() as directory:
            snapshot = Path(directory) / '.luna-stream-example.conf'
            snapshot.write_text('[Moonlight]')
            clock = {'now': 0.0}
            calls = []
            def simulate_systemd(command, **kwargs):
                calls.append(command)
                receipt = next(arg.split('=', 1)[1] for arg in command
                               if arg.startswith('LUNA_LAUNCH_ACK='))
                Path(receipt).write_text('reconciling:1\n')
                return complete
            def advance(_delay):
                clock['now'] += 80
            with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                            side_effect=simulate_systemd), \
                 mock.patch('resources.lib.util.moonlighthelper.time.monotonic',
                            side_effect=lambda: clock['now']), \
                 mock.patch('resources.lib.util.moonlighthelper.time.sleep',
                            side_effect=advance):
                ok, error = helper._launch_coreelec_ng_service(
                    'Desktop', '/addon/resources/launchscripts/coreelec/',
                    {'LUNA_STREAM_CONFIG': str(snapshot)})
            self.assertIsNone(ok)
            self.assertIn('abandoned', error)
            self.assertEqual(len(calls), 1)
            self.assertTrue(snapshot.exists())
            receipts = [p for p in Path(directory).glob('.luna-ack-*')
                        if not p.name.endswith('.abandoned')]
            self.assertEqual(len(receipts), 1)
            self.assertEqual(receipts[0].read_text().strip(), 'abandoned')
            self.assertEqual(Path(str(receipts[0]) + '.abandoned')
                             .read_text().strip(), 'abandoned')

    def test_late_arm_at_timeout_can_still_be_observed(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        done = subprocess.CompletedProcess(args=[], returncode=0, stdout='')
        with tempfile.TemporaryDirectory() as directory:
            snapshot = Path(directory) / '.luna-stream-test.conf'
            snapshot.write_text('[Moonlight]')
            clock = {'now': 0}
            def queue(command, **_kwargs):
                receipt = next(item.split('=', 1)[1] for item in command
                               if item.startswith('LUNA_LAUNCH_ACK='))
                Path(receipt).write_text('reconciling:1\n')
                return done
            def advance(_delay):
                clock['now'] += 80
                next(Path(directory).glob('.luna-ack-*')).write_text('armed\n')
            with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                            side_effect=queue), \
                 mock.patch('resources.lib.util.moonlighthelper.time.monotonic',
                            side_effect=lambda: clock['now']), \
                 mock.patch('resources.lib.util.moonlighthelper.time.sleep',
                            side_effect=advance):
                ok, error = helper._launch_coreelec_ng_service(
                    'Desktop', '/addon/resources/launchscripts/coreelec/',
                    {'LUNA_STREAM_CONFIG': str(snapshot)})
            self.assertTrue(ok, error)
            self.assertEqual(next(Path(directory).glob('.luna-ack-*'))
                             .read_text().strip(), 'observed')

    def test_unacknowledged_launcher_is_stopped_before_reporting_failure(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        with tempfile.TemporaryDirectory() as directory:
            snapshot = Path(directory) / '.luna-stream-example.conf'
            snapshot.write_text('[Moonlight]')
            requests = []

            def fake_run(command, **kwargs):
                requests.append(command)
                if command[1:3] == ['show', '--property=ActiveState']:
                    return subprocess.CompletedProcess(command, 0, stdout='inactive\n')
                return subprocess.CompletedProcess(command, 0, stdout='')

            with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                            side_effect=fake_run), \
                 mock.patch('resources.lib.util.moonlighthelper.LUNA_NG_ACK_WAIT_SECONDS',
                            0.0):
                ok, error = helper._launch_coreelec_ng_service(
                    'Desktop', '/addon/resources/launchscripts/coreelec/',
                    {'LUNA_STREAM_CONFIG': str(snapshot)})
            self.assertFalse(ok)
            self.assertIn('did not acknowledge', error)
            self.assertTrue(any(len(item) >= 3 and item[1] == 'stop'
                                for item in requests))
            self.assertFalse(list(Path(directory).glob('.luna-ack-*')))
            self.assertTrue(snapshot.exists())  # Caller retires it only after failure.

    def test_uncertain_launcher_ownership_preserves_receipt(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        with tempfile.TemporaryDirectory() as directory:
            snapshot = Path(directory) / '.luna-stream-example.conf'
            snapshot.write_text('[Moonlight]')
            def fake_run(command, **kwargs):
                if command[1:3] == ['show', '--property=ActiveState']:
                    return subprocess.CompletedProcess(command, 0, stdout='active\n')
                if len(command) > 1 and command[1] == 'stop':
                    return subprocess.CompletedProcess(command, 1, stdout='denied')
                return subprocess.CompletedProcess(command, 0, stdout='')

            with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                            side_effect=fake_run), \
                 mock.patch('resources.lib.util.moonlighthelper.LUNA_NG_ACK_WAIT_SECONDS',
                            0.0):
                ok, error = helper._launch_coreelec_ng_service(
                    'Desktop', '/addon/resources/launchscripts/coreelec/',
                    {'LUNA_STREAM_CONFIG': str(snapshot)})
            self.assertIsNone(ok)
            self.assertIn('not acknowledged', error)
            self.assertTrue(list(Path(directory).glob('.luna-ack-*')))
            self.assertTrue(snapshot.exists())

    def test_systemd_handoff_forwards_bounded_sdl_mapping_override(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        completed = subprocess.CompletedProcess(args=[], returncode=0, stdout='')
        env = {
            'SDL_GAMECONTROLLERCONFIG': '030000005e0400008e02000014010000,Pad,a:b0,b:b1,',
            'UNRELATED_SECRET': 'not-forwarded'
        }
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                        return_value=completed) as runner:
            ok, error = helper._launch_coreelec_ng_service(
                'Desktop', '/addon/resources/launchscripts/coreelec/', env)
        self.assertTrue(ok)
        self.assertIsNone(error)
        command = runner.call_args.args[0]
        self.assertIn('SDL_GAMECONTROLLERCONFIG=' + env['SDL_GAMECONTROLLERCONFIG'],
                      command)
        self.assertFalse(any('UNRELATED_SECRET=' in a for a in command))

    def test_systemd_handoff_rejects_oversized_sdl_mapping_override(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run') as runner:
            ok, error = helper._launch_coreelec_ng_service(
                'Desktop', '/addon/resources/launchscripts/coreelec/',
                {'SDL_GAMECONTROLLERCONFIG': 'x' * 16385})
        self.assertFalse(ok)
        self.assertIn('16 KiB', error)
        runner.assert_not_called()

    def test_quit_targets_confirmed_host_instead_of_cached_configuration(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.config_helper = types.SimpleNamespace(
            binary_path='/addon/bin/coreelec-ng/', addon_path='/addon/')
        helper.logger = types.SimpleNamespace(info=lambda *_: None)
        completed = subprocess.CompletedProcess(args=[], returncode=0,
                                                stdout='', stderr='')
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                        return_value=completed) as runner:
            self.assertTrue(helper.quit_game('host-A'))
            self.assertEqual(runner.call_args.args[0],
                             ['/addon/bin/coreelec-ng/moonlight', 'quit', 'host-A'])
            self.assertEqual(runner.call_args.kwargs['cwd'],
                             '/addon/bin/coreelec-ng/')
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run') as runner:
            self.assertFalse(helper.quit_game('-bad-host'))
            self.assertFalse(helper.quit_game(''))
            runner.assert_not_called()

    def test_unpair_requires_confirmed_host_state(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.config_helper = types.SimpleNamespace(
            binary_path='/addon/bin/coreelec-ng/',
            addon_path='/addon/',
            config={'host_addr': 'host-A'},
            configure=lambda: None)
        helper.last_unpair_error = None
        native = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='Succesfully unpaired', stderr='')
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                        return_value=native) as runner, \
             mock.patch('resources.lib.util.hostapi.Host.has_trusted_pin',
                        return_value=True), \
             mock.patch('resources.lib.util.hostapi.Host.pair_status',
                        return_value=True):
            self.assertFalse(helper.unpair())
            self.assertIn('still reports', helper.last_unpair_error)
            self.assertEqual(runner.call_args.args[0],
                             ['/addon/bin/coreelec-ng/moonlight', 'unpair', 'host-A'])
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                        return_value=native), \
             mock.patch('resources.lib.util.hostapi.Host.has_trusted_pin',
                        return_value=True), \
             mock.patch('resources.lib.util.hostapi.Host.pair_status',
                        return_value=False):
            self.assertTrue(helper.unpair())
            self.assertFalse(helper.last_unpair_unverified)

    def test_failed_native_unpair_cannot_be_rescued_by_public_status(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.config_helper = types.SimpleNamespace(
            binary_path='/addon/bin/coreelec-ng/', addon_path='/addon/',
            config={'host_addr': 'host-A'}, configure=lambda: None)
        native = subprocess.CompletedProcess(
            args=[], returncode=1, stdout='Unpair unsupported', stderr='')
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                        return_value=native), \
             mock.patch('resources.lib.util.hostapi.Host.pair_status',
                        return_value=False) as pair_status:
            self.assertFalse(helper.unpair())
            self.assertIn('exit 1', helper.last_unpair_error)
            pair_status.assert_not_called()

    def test_unpair_does_not_guess_when_status_cannot_be_checked(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.config_helper = types.SimpleNamespace(
            binary_path='/addon/bin/coreelec-ng/',
            addon_path='/addon/',
            config={'host_addr': 'host-A'},
            configure=lambda: None)
        helper.last_unpair_error = None
        native = subprocess.CompletedProcess(
            args=[], returncode=0, stdout='Succesfully unpaired', stderr='')
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                        return_value=native), \
             mock.patch('resources.lib.util.hostapi.Host.has_trusted_pin',
                        return_value=True), \
             mock.patch('resources.lib.util.hostapi.Host.pair_status',
                        side_effect=OSError('unavailable')):
            self.assertFalse(helper.unpair())
            self.assertIn('Cannot confirm', helper.last_unpair_error)

    def test_revoked_tls_unpair_uses_public_status_only_as_unverified(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.config_helper = types.SimpleNamespace(
            binary_path='/addon/bin/coreelec-ng/', addon_path='/addon/',
            config={'host_addr': 'host-A'}, configure=lambda: None)
        native = subprocess.CompletedProcess(args=[], returncode=0,
                                             stdout='Succesfully unpaired')
        def public_fallback(host, allow_http_fallback=False):
            self.assertTrue(allow_http_fallback)
            host._last_pair_status_public = True
            return False
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                        return_value=native), \
             mock.patch('resources.lib.util.hostapi.Host.has_trusted_pin',
                        return_value=True), \
             mock.patch('resources.lib.util.hostapi.Host.pair_status',
                        new=public_fallback):
            self.assertTrue(helper.unpair())
            self.assertTrue(helper.last_unpair_unverified)

    def test_unpinned_native_unpair_is_reported_as_unverified(self):
        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.config_helper = types.SimpleNamespace(
            binary_path='/addon/bin/coreelec-ng/', addon_path='/addon/',
            config={'host_addr': 'host-A'}, configure=lambda: None)
        native = subprocess.CompletedProcess(args=[], returncode=0,
                                             stdout='Succesfully unpaired')
        with mock.patch('resources.lib.util.moonlighthelper.subprocess.run',
                        return_value=native), \
             mock.patch('resources.lib.util.hostapi.Host.has_trusted_pin',
                        return_value=False), \
             mock.patch('resources.lib.util.hostapi.Host.pair_status') as verify:
            self.assertTrue(helper.unpair())
            self.assertTrue(helper.last_unpair_unverified)
            self.assertIsNone(helper.last_unpair_error)
            verify.assert_not_called()

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
                    'resources.lib.util.confighelper.inputdevices.resolve_selectors',
                    return_value=['/dev/input/event7', '/dev/input/event8']) as resolve:
                helper._dump_conf()

            resolve.assert_called_once_with([
                {'name': 'Keyboard', 'kind': 'N', 'value': ''},
                {'name': 'Mouse', 'kind': 'N', 'value': ''}])
            text = config_path.read_text(encoding='utf-8')
            self.assertIn('input = /dev/input/event7\n', text)
            self.assertIn('input = /dev/input/event8\n', text)
            self.assertNotIn('input = Keyboard\n', text)
            self.assertNotIn('input = Mouse\n', text)


if __name__ == '__main__':
    unittest.main()
