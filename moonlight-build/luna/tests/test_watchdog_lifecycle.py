"""Fault-injection tests for the independent CoreELEC Kodi recovery watchdog."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

WATCHDOG = (Path(__file__).resolve().parents[1] /
            'resources/launchscripts/coreelec/kodi-watchdog.sh')

SYSTEMCTL = r'''#!/bin/sh
printf '%s\n' "$*" >> "$MOCK_ROOT/calls"
case "$1" in
  show)
    case "$*" in
      *ActiveState*)
        if [ "${HANG_PROBE:-0}" = 1 ] && [ ! -f "$MOCK_ROOT/hung" ]; then
          touch "$MOCK_ROOT/hung"
          # Replace this shell rather than orphaning a child that holds the
          # captured test pipe open after timeout kills its parent.
          exec sleep 60
        fi
        case "$*" in
          *kodi.service*)
            if [ "${FLIP_KODI_SECOND_READ:-0}" = 1 ]; then
              reads=$(cat "$MOCK_ROOT/kodi-reads" 2>/dev/null || echo 0)
              reads=$((reads + 1))
              printf '%s\n' "$reads" > "$MOCK_ROOT/kodi-reads"
              [ "$reads" -lt 2 ] || printf 'active\n' > "$MOCK_ROOT/kodi"
            fi
            cat "$MOCK_ROOT/kodi" ;;
          *) cat "$MOCK_ROOT/sessionstate" ;;
        esac; exit 0 ;;
      *MainPID*)
        if [ -r "$MOCK_SESSION_OWNER" ]; then
          cut -d '|' -f 1 "$MOCK_SESSION_OWNER"
        else
          echo 0
        fi
        exit 0 ;;
      *ControlGroup*) exit 0 ;;
    esac ;;
  is-active)
    if [ "$3" = kodi.service ]; then
      [ "$(cat "$MOCK_ROOT/kodi")" = active ] && exit 0
      exit 3
    fi ;;
  stop)
    printf '%s\n' "${STOP_STATE:-inactive}" > "$MOCK_ROOT/sessionstate"
    exit 0 ;;
  start)
    if [ -n "${WORKER_PID:-}" ] && kill -0 "$WORKER_PID" 2>/dev/null; then
      echo 'KODI STARTED WITH WORKER' >> "$MOCK_ROOT/calls"
    fi
    printf 'active\n' > "$MOCK_ROOT/kodi"
    exit 0 ;;
esac
exit 1
'''


class WatchdogLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        systemctl = self.root / 'systemctl'
        systemctl.write_text(SYSTEMCTL)
        systemctl.chmod(0o755)
        (self.root / 'kodi').write_text('active\n')
        (self.root / 'sessionstate').write_text('inactive\n')
        self.session = self.root / '.luna-session.test1234'
        self.session.write_text('1\n')
        Path(str(self.session) + '.native-not-started').write_text('not-started\n')
        Path(str(self.session) + '.kodi-initial').write_text('active\n')
        owner_pid = os.getpid()
        owner_stat = Path('/proc/%d/stat' % owner_pid).read_text()
        owner_start = owner_stat.rsplit(') ', 1)[1].split()[19]
        Path(str(self.session) + '.owner').write_text('%d|%s\n' % (owner_pid, owner_start))
        self.env = dict(os.environ, MOCK_ROOT=str(self.root),
                        MOCK_SESSION_OWNER=str(self.session) + '.owner',
                        LUNA_SYSTEMCTL=str(systemctl), LUNA_STARTUP_TIMEOUT='5')
        for key in ('LUNA_VDEC_STATUS', 'LUNA_FB1_BLANK'):
            self.env[key] = str(self.root / 'absent')
        video = self.root / 'video_disable'
        fb0 = self.root / 'fb0_blank'
        video.write_text('0')
        fb0.write_text('1')
        self.env['LUNA_VIDEO_DISABLE'] = str(video)
        self.env['LUNA_FB0_BLANK'] = str(fb0)
        (self.root / 'cpufreq/policy0').mkdir(parents=True)
        (self.root / 'net-core').mkdir()
        self.env['LUNA_CPUFREQ_ROOT'] = str(self.root / 'cpufreq')
        self.env['LUNA_NET_CORE_ROOT'] = str(self.root / 'net-core')
        self.env['LUNA_TSYNC_ENABLE'] = str(self.root / 'tsync_enable')

    def run_watchdog(self, *args, timeout=13):
        return subprocess.run(['sh', str(WATCHDOG), 'luna-ng-session.service',
                               str(self.session), *map(str, args)], env=self.env,
                              text=True, capture_output=True, timeout=timeout)

    def test_deactivating_session_does_not_return_display_to_kodi(self):
        (self.root / 'kodi').write_text('inactive\n')
        (self.root / 'sessionstate').write_text('active\n')
        (self.root / 'session.startup').write_text(str(int(time.time()) - 30))
        worker = subprocess.Popen(['sleep', '30'])
        self.env.update(STOP_STATE='deactivating', WORKER_PID=str(worker.pid))
        process = subprocess.Popen(
            ['sh', str(WATCHDOG), 'luna-ng-session.service', str(self.session)],
            env=self.env, start_new_session=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            time.sleep(1.8)
            self.assertIsNone(process.poll(), 'watchdog treated deactivating as stopped')
            calls = (self.root / 'calls').read_text()
            self.assertNotIn('KODI STARTED WITH WORKER', calls)
            self.assertNotIn('start kodi.service', calls)
            self.assertTrue(self.session.exists())
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
            worker.terminate()
            worker.wait()

    def test_blocking_systemd_status_is_bounded(self):
        (self.root / 'kodi').write_text('inactive\n')
        (self.root / 'sessionstate').write_text('active\n')
        (self.root / 'session.startup').write_text(str(int(time.time()) - 30))
        self.env.update(HANG_PROBE='1', STOP_STATE='inactive')
        # The first systemctl probe hangs for 60 seconds. The watchdog must
        # bound that query and recover, rather than wait for the fake service.
        started = time.monotonic()
        result = self.run_watchdog(timeout=25)
        self.assertLess(time.monotonic() - started, 25)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = (self.root / 'calls').read_text()
        self.assertIn('stop --no-block luna-ng-session.service', calls)
        self.assertIn('start kodi.service', calls)
        self.assertFalse(self.session.exists())

    def test_failed_tuning_restores_are_retried_before_disarm(self):
        governor = Path(str(self.session) + '.governor-state')
        network = Path(str(self.session) + '.network-state')
        gov_node = self.root / 'cpufreq/policy0/scaling_governor'
        net_node = self.root / 'net-core/rmem_max'
        governor.write_text(str(gov_node) + '|schedutil\n')
        network.write_text(str(net_node) + '|212992|33554432\n')
        first = self.run_watchdog(governor, network)
        self.assertEqual(first.returncode, 1, first.stderr)
        self.assertTrue(governor.exists() and network.exists())
        self.assertTrue(self.session.exists())
        gov_node.write_text('performance')
        net_node.write_text('33554432')
        retry = self.run_watchdog(governor, network)
        self.assertEqual(retry.returncode, 0, retry.stderr)
        self.assertEqual(gov_node.read_text(), 'schedutil')
        self.assertEqual(net_node.read_text(), '212992')
        self.assertFalse(governor.exists() or network.exists() or self.session.exists())

    def test_initially_inactive_kodi_is_not_started_after_recovery(self):
        (self.root / 'kodi').write_text('inactive\n')
        Path(str(self.session) + '.kodi-initial').write_text('inactive\n')
        result = self.run_watchdog()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / 'kodi').read_text().strip(), 'inactive')
        calls = (self.root / 'calls').read_text()
        self.assertNotIn('start kodi.service', calls)
        self.assertFalse(self.session.exists())

    def test_invalid_session_arm_marker_keeps_recovery_journals(self):
        (self.root / 'kodi').write_text('inactive\n')
        for invalid in ('', 'corrupt\n', '0\n\n', '1\n\n'):
            with self.subTest(value=invalid):
                self.session.write_text(invalid)
                result = self.run_watchdog()
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertTrue(self.session.exists())
                self.assertNotIn('start kodi.service',
                                 (self.root / 'calls').read_text())

    def test_invalid_native_receipt_blocks_display_recovery(self):
        receipt = Path(str(self.session) + '.native-not-started')
        for value in ('', 'not started', 'unverified', 'not-started', 'not-started\n\n'):
            with self.subTest(receipt=value):
                self.session.write_text('1\n')
                receipt.write_text(value)
                (self.root / 'kodi').write_text('inactive\n')
                result = self.run_watchdog()
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertTrue(self.session.exists())
        self.assertNotIn('start kodi.service', (self.root / 'calls').read_text())

    def test_retired_session_is_a_successful_noop(self):
        self.session.unlink()
        result = self.run_watchdog()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root / 'calls').exists())

    def test_blocking_sysfs_restore_does_not_prevent_kodi_recovery(self):
        for kind, position in [('tsync', 2), ('network', 1), ('governor', 0)]:
            with self.subTest(kind=kind):
                self.session.write_text('1\n')
                (self.root / 'kodi').write_text('inactive\n')
                journal = Path(str(self.session) + '.' + kind + '-state')
                pipe = {
                    'governor': self.root / 'cpufreq/policy0/scaling_governor',
                    'network': self.root / 'net-core/rmem_max',
                    'tsync': self.root / 'tsync_enable',
                }[kind]
                os.mkfifo(pipe)
                journal.write_text(str(pipe) + (
                    '|schedutil\n' if kind == 'governor' else '|1|0\n'))
                args = ['', '', '']
                args[position] = str(journal)
                self.env.update(LUNA_RECOVERY_TIMEOUT='2',
                                LUNA_RECOVERY_HELPER_TIMEOUT='1')
                result = self.run_watchdog(*args, timeout=7)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual((self.root / 'kodi').read_text().strip(), 'active')
                self.assertTrue(journal.exists())
                self.assertTrue(self.session.exists())
                self.assertIn('timed out/failed', result.stderr)

    def test_old_watchdog_does_not_stop_new_session_unit(self):
        (self.root / 'kodi').write_text('inactive\n')
        (self.root / 'sessionstate').write_text('active\n')
        (self.root / 'session.startup').write_text(str(int(time.time()) - 30))
        Path(str(self.session) + '.owner').write_text('999999999|10\n')
        result = self.run_watchdog()
        self.assertEqual(result.returncode, 1, result.stderr)
        calls = (self.root / 'calls').read_text()
        self.assertNotIn('stop --no-block', calls)
        self.assertNotIn('start kodi.service', calls)
        self.assertTrue(self.session.exists())

    def test_recover_only_never_stops_a_live_session(self):
        (self.root / 'sessionstate').write_text('active\n')
        self.env['LUNA_RECOVER_ONLY'] = '1'
        # A recover-only invocation bypasses monitor and can recover retired
        # state only after the old owner/lock have been checked by the caller.
        result = self.run_watchdog()
        self.assertEqual(result.returncode, 1, result.stderr)
        calls = (self.root / 'calls').read_text() if (self.root / 'calls').exists() else ''
        self.assertNotIn('stop --no-block', calls)
        self.assertTrue(self.session.exists())

    def test_unverified_marker_cannot_clear_without_video_evidence(self):
        marker = Path(str(self.session) + '.display-pending')
        marker.write_text('unverified\n')
        result = self.run_watchdog()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists() and self.session.exists())

    def test_kodi_becoming_active_between_recovery_steps_blocks_writes(self):
        # The watchdog first observes inactive Kodi, then an independent
        # Kodi start happens while it restores HDMI/AML/network state.
        (self.root / 'kodi').write_text('inactive\n')
        self.env['FLIP_KODI_SECOND_READ'] = '1'
        pending = Path(str(self.session) + '.display-pending')
        pending.write_text('unverified\n')
        result = self.run_watchdog(timeout=14)
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / 'video_disable').read_text(), '0')
        self.assertEqual((self.root / 'fb0_blank').read_text(), '1')
        self.assertTrue(pending.exists())
        self.assertTrue(self.session.exists())

    def test_unknown_kodi_state_cannot_reset_display(self):
        (self.root / 'kodi').write_text('deactivating\n')
        video = self.root / 'video'
        video.write_text('0')
        self.env.update(LUNA_VIDEO_DISABLE=str(video),
                        LUNA_RECOVERY_TIMEOUT='2', LUNA_RECOVERY_HELPER_TIMEOUT='1')
        result = self.run_watchdog(timeout=14)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(video.read_text(), '0')
        self.assertTrue(self.session.exists())

    def test_kodi_active_with_recovered_fb_clears_display_marker(self):
        marker = Path(str(self.session) + '.display-pending')
        marker.write_text('fb0\n')
        fb0 = self.root / 'fb0-blank'
        fb1 = self.root / 'fb1-blank'
        fb0.write_text('0')
        fb1.write_text('0')
        self.env['LUNA_FB0_BLANK'] = str(fb0)
        self.env['LUNA_FB1_BLANK'] = str(fb1)
        self.assertEqual(self.run_watchdog().returncode, 0)
        self.assertFalse(marker.exists())
        self.assertFalse(self.session.exists())

    def test_pending_display_marker_never_writes_active_kodi_fb(self):
        marker = Path(str(self.session) + '.display-pending')
        marker.write_text('fb0\n')
        fb0 = self.root / 'fb0-blank'
        fb0.write_text('1')
        self.env['LUNA_FB0_BLANK'] = str(fb0)
        result = self.run_watchdog()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(fb0.read_text(), '1')
        self.assertTrue(marker.exists())
        self.assertTrue(self.session.exists())
        fb0.write_text('0')
        result = self.run_watchdog()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(marker.exists())

    def test_unsafe_watchdog_journal_argument_does_not_touch_other_files(self):
        other = self.root / 'unrelated'
        other.write_text('unchanged')
        self.env['LUNA_RECOVERY_TIMEOUT'] = '2'
        result = self.run_watchdog(other)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(other.read_text(), 'unchanged')
        self.assertTrue(self.session.exists())
        self.assertNotIn('start kodi.service',
                         (self.root / 'calls').read_text() if (self.root / 'calls').exists() else '')

    def test_external_tuning_changes_are_not_overwritten(self):
        governor = Path(str(self.session) + '.governor-state')
        node = self.root / 'cpufreq/policy0/scaling_governor'
        governor.write_text(str(node) + '|schedutil\n')
        node.write_text('powersave')
        result = self.run_watchdog(governor)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(node.read_text(), 'powersave')
        self.assertFalse(governor.exists())


if __name__ == '__main__':
    unittest.main()
