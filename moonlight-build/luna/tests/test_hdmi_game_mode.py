import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from xml.etree import ElementTree

from resources.lib.util import hdmigamemode as hdmi

ROOT = Path(__file__).resolve().parents[1]


class HdmiGameModeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.state = self.root / 'snapshot'
        for name, value in (('allm_cap', '1'), ('allm_mode', '0'), ('contenttype_mode', 'cinema')):
            (self.root / name).write_text(value + '\n')
        self.hardware_content = 'cinema'
        self.writes = []
        def write(root, name, value):
            self.writes.append((name, value))
            (root / name).write_text(value + '\n')
            if name == 'allm_mode' and value == '1':
                self.assertTrue(self.state.stat().st_size, 'snapshot must precede HDMI writes')
                self.hardware_content = 'off'
            if name == 'contenttype_mode':
                (root / 'allm_mode').write_text('0\n')
                self.hardware_content = value
        patch = mock.patch.object(hdmi, 'write', side_effect=write)
        patch.start()
        self.addCleanup(patch.stop)

    def test_setting_is_enabled_by_default(self):
        settings = ElementTree.parse(ROOT / 'resources/settings.xml')
        setting = next(s for s in settings.iter('setting') if s.get('id') == 'tv_game_mode')
        self.assertEqual(setting.findtext('default'), 'true')

    def test_all_previous_content_modes_restore_including_stale_readback(self):
        for content in hdmi.CONTENT_TYPES:
            with self.subTest(content=content):
                (self.root / 'contenttype_mode').write_text(content)
                hdmi.enable(self.state, self.root)
                self.assertEqual(hdmi.read(self.root, 'allm_mode'), '1')
                self.assertEqual(self.hardware_content, 'off')
                hdmi.restore(self.state)
                self.assertEqual(hdmi.read(self.root, 'allm_mode'), '0')
                self.assertEqual(self.hardware_content, content)
                self.assertFalse(self.state.exists())
                hdmi.restore(self.state)

    def test_already_enabled_and_unsupported_displays_are_untouched(self):
        for capability, current in (('1', '1'), ('0', '0')):
            (self.root / 'allm_cap').write_text(capability)
            (self.root / 'allm_mode').write_text(current)
            hdmi.enable(self.state, self.root)
            hdmi.restore(self.state)
            self.assertEqual(self.writes, [])

    def test_external_content_change_is_preserved(self):
        hdmi.enable(self.state, self.root)
        (self.root / 'contenttype_mode').write_text('photo')
        (self.root / 'allm_mode').write_text('0')
        hdmi.restore(self.state)
        self.assertEqual(hdmi.read(self.root, 'contenttype_mode'), 'photo')
        self.assertEqual(self.writes, [('allm_mode', '1')])

    def test_external_content_change_while_allm_enabled_retains_snapshot(self):
        hdmi.enable(self.state, self.root)
        (self.root / 'contenttype_mode').write_text('photo')
        with self.assertRaisesRegex(OSError, 'ALLM still enabled'):
            hdmi.restore(self.state)
        self.assertTrue(self.state.exists())
        self.assertEqual(hdmi.read(self.root, 'allm_mode'), '1')
        self.assertEqual(hdmi.read(self.root, 'contenttype_mode'), 'photo')
        # Once an external owner disables ALLM, retire the marker without
        # changing the external content hint.
        (self.root / 'allm_mode').write_text('0')
        hdmi.restore(self.state)
        self.assertFalse(self.state.exists())
        self.assertEqual(hdmi.read(self.root, 'contenttype_mode'), 'photo')

    def test_restore_rejects_modified_hardware_root(self):
        hdmi.enable(self.state, self.root)
        wrong = self.root / 'unrelated'
        wrong.mkdir()
        for name in ('allm_mode', 'contenttype_mode'):
            (wrong / name).write_text('untouched')
        with self.assertRaisesRegex(ValueError, 'trusted output'):
            hdmi.restore(self.state, expected_root=wrong)
        self.assertTrue(self.state.exists())
        self.assertEqual((wrong / 'allm_mode').read_text(), 'untouched')
        hdmi.restore(self.state, expected_root=self.root)
        self.assertFalse(self.state.exists())

    def test_failed_enable_rollback_cannot_follow_modified_snapshot_root(self):
        foreign = self.root / 'foreign'
        foreign.mkdir()
        for name in ('allm_mode', 'contenttype_mode'):
            (foreign / name).write_text('untouched')
        initial_write = hdmi.write

        def corrupt_snapshot(root, name, value):
            if name == 'allm_mode' and value == '1':
                saved = json.loads(self.state.read_text())
                saved['root'] = str(foreign)
                self.state.write_text(json.dumps(saved))
                raise OSError('simulated HDMI activation failure')
            return initial_write(root, name, value)

        with mock.patch.object(hdmi, 'write', side_effect=corrupt_snapshot):
            with self.assertRaisesRegex(ValueError, 'trusted output'):
                hdmi.enable(self.state, self.root)
        self.assertEqual((foreign / 'allm_mode').read_text(), 'untouched')
        self.assertEqual((foreign / 'contenttype_mode').read_text(), 'untouched')
        self.assertTrue(self.state.exists())  # Unsafe snapshot stays inspectable.

    def test_failed_enable_restores_before_returning(self):
        real_write = hdmi.write
        def fail_enable(root, name, value):
            if name == 'allm_mode':
                raise OSError('injected HDMI failure')
            real_write(root, name, value)
        with mock.patch.object(hdmi, 'write', side_effect=fail_enable):
            with self.assertRaisesRegex(OSError, 'injected'):
                hdmi.enable(self.state, self.root)
        self.assertEqual(hdmi.read(self.root, 'allm_mode'), '0')
        self.assertFalse(self.state.exists())


class HdmiLauncherTests(unittest.TestCase):
    """Run the real launcher/watchdog against emulated services and HDMI sysfs."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.hdmi = self.root / 'hdmi'
        self.hdmi.mkdir()
        for name, value in (('allm_cap', '1'), ('allm_mode', '0'), ('contenttype_mode', 'cinema')):
            (self.hdmi / name).write_text(value)
        self.calls = self.root / 'calls'
        self.calls.touch()
        scripts = {
            'systemctl': '''#!/bin/sh
kodi_state() {
  if [ -f "$CALLS.kodi" ]; then
    cat "$CALLS.kodi"
  elif [ "${TEST_KODI_ACTIVE:-1}" = 1 ]; then
    echo active
  else
    echo inactive
  fi
}
case "$1" in
is-active) [ "$3" = kodi.service ] && [ "$(kodi_state)" = active ] ;;
show) case "$*" in
   *ActiveState*) case "$*" in *kodi.service*) kodi_state ;; *) echo inactive ;; esac ;;
esac ;;
start)
  printf 'kodi:%s:%s\\n' "$(cat "$LUNA_HDMI_ROOT/allm_mode")" "$(cat "$LUNA_HDMI_ROOT/contenttype_mode")" >> "$CALLS"
  printf 'active\\n' > "$CALLS.kodi"
  : > "$CALLS.started" ;;
stop) printf 'inactive\\n' > "$CALLS.kodi"; rm -f "$CALLS.started" ;;
esac
''',
            'killall': '#!/bin/sh\nexit 0\n',
            'systemd-run': '''#!/bin/sh
for arg in "$@"; do
 case "$arg" in */.luna-session.*) : > "$arg.ready" ;; esac
done
''',
            'moonlight': '''#!/bin/sh
printf 'stream:%s\\n' "$(cat "$LUNA_HDMI_ROOT/allm_mode")" >> "$CALLS"
exit "${TEST_STREAM_EXIT:-42}"
''',
            'python3': '''#!%s
import importlib.util, os, sys
if sys.argv[1].endswith('command-timeout.py'):
    os.execv(sys.executable, [sys.executable] + sys.argv[1:])
if sys.argv[1].endswith('vfmruntime.py'):
    sys.exit(0)  # successful VFM activation, without touching host devices
spec = importlib.util.spec_from_file_location('hdmi', sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
original_write = m.write
def write(root, name, value):
    original_write(root, name, value)
    if name == 'contenttype_mode':
        original_write(root, 'allm_mode', '0')
m.write = write
if sys.argv[2] == 'enable':
    m.enable(sys.argv[3], sys.argv[4])
else:
    if os.environ.get('TEST_HDMI_RESTORE_FAIL') == '1':
        sys.exit(1)
    m.restore(sys.argv[3])
''' % sys.executable,
        }
        for name, body in scripts.items():
            path = self.root / name
            path.write_text(body)
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.root) + ':' + os.environ['PATH'], CALLS=str(self.calls),
                        LUNA_LOG_DIR=str(self.root / 'logs'), LUNA_HDMI_ROOT=str(self.hdmi),
                        MOONLIGHT_TV_GAME_MODE='1', MOONLIGHT_BINARY=str(self.root / 'moonlight'),
                        LUNA_SYSTEMCTL=str(self.root / 'systemctl'), LUNA_SYSTEMD_RUN=str(self.root / 'systemd-run'),
                        LUNA_KILLALL=str(self.root / 'killall'), MOONLIGHT_CPU_PERFORMANCE='0')
        for key in ('LUNA_VDEC_STATUS', 'LUNA_FB1_BLANK',
                    'LUNA_CPUFREQ_ROOT', 'LUNA_NET_CORE_ROOT', 'LUNA_TSYNC_ENABLE'):
            self.env[key] = str(self.root / 'absent')
        video = self.root / 'video_disable'
        fb0 = self.root / 'fb0_blank'
        video.write_text('0')
        fb0.write_text('1')
        self.env['LUNA_VIDEO_DISABLE'] = str(video)
        self.env['LUNA_FB0_BLANK'] = str(fb0)

    def test_both_pipelines_restore_before_kodi_after_native_failure(self):
        for pipeline, exit_code in (('aml', 0), ('aml', 42), ('aml-vfm', 0), ('aml-vfm', 42)):
            self.env['TEST_STREAM_EXIT'] = str(exit_code)
            self.calls.write_text('')
            self.env.update(MOONLIGHT_VIDEO_PIPELINE=pipeline, MOONLIGHT_VFM_ROOT='/unused')
            result = subprocess.run(['sh', str(ROOT / 'resources/launchscripts/coreelec/launch.sh'), 'Desktop'],
                                    env=self.env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, exit_code, result.stdout + result.stderr)
            self.assertEqual(self.calls.read_text().splitlines(), ['stream:1', 'kodi:0:cinema'])

    def test_launcher_preserves_recovery_marker_when_hdmi_restore_fails(self):
        self.env['TEST_STREAM_EXIT'] = '42'
        self.env['TEST_HDMI_RESTORE_FAIL'] = '1'
        self.env['MOONLIGHT_VIDEO_PIPELINE'] = 'aml'
        result = subprocess.run(
            ['sh', str(ROOT / 'resources/launchscripts/coreelec/launch.sh'), 'Desktop'],
            env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 42, result.stdout + result.stderr)
        self.assertIn('stream:1', self.calls.read_text())
        states = list((self.root / 'logs').glob('.luna-session.*'))
        armed = [state for state in states
                 if state.is_file() and state.name.count('.') == 2]
        self.assertEqual(len(armed), 1, states)
        self.assertEqual(armed[0].read_text().strip(), '1')
        snapshots = list((self.root / 'logs').glob('.luna-session.*.hdmi-state'))
        self.assertTrue(any(snapshot.stat().st_size > 0 for snapshot in snapshots))

    def test_watchdog_restores_before_restarting_kodi(self):
        # Kodi was stopped by Luna; unlike the launcher test, it is not active.
        self.env['TEST_KODI_ACTIVE'] = '0'
        session = self.root / '.luna-session.test1234'
        state = Path(str(session) + '.hdmi-state')
        state.write_text(json.dumps({'schema': 1, 'root': str(self.hdmi), 'allm': '0', 'content': 'cinema'}))
        (self.hdmi / 'allm_mode').write_text('1')
        session.write_text('1\n')
        Path(str(session) + '.native-not-started').write_text('not-started\n')
        result = subprocess.run(['sh', str(ROOT / 'resources/launchscripts/coreelec/kodi-watchdog.sh'),
                                 'luna-ng-session.service', str(session), '', '', '', str(state)],
                                env=self.env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.calls.read_text().strip(), 'kodi:0:cinema')
        self.assertFalse(state.exists())


if __name__ == '__main__':
    unittest.main()
