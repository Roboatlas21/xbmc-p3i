"""End-to-end recovery tests using real launcher/watchdog and fake services."""
import os
import time
import fcntl
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / 'resources/launchscripts/coreelec'

SYSTEMCTL = '''#!/bin/sh
printf '%s\n' "$*" >> "$MOCK_CALLS"
case "$1" in
 is-active) [ "$(cat "$MOCK_KODI")" = active ] ;;
 show) case "$*" in
   *ActiveState*) case "$*" in
       *kodi.service*) cat "$MOCK_KODI" ;;
       *) printf '%s\n' "${MOCK_OLD_UNIT_STATE:-inactive}" ;;
     esac ;;
   *ControlGroup*) printf '%s\n' "${MOCK_OLD_CGROUP:-}" ;;
 esac ;;
 stop) echo inactive > "$MOCK_KODI" ;;
 start) echo active > "$MOCK_KODI" ;;
esac
'''
SYSTEMD_RUN = '''#!/bin/sh
[ "${FAIL_WATCHDOG:-0}" = 1 ] && exit 1
for arg in "$@"; do
 case "$arg" in */.luna-session.*) : > "$arg.ready"; break ;; esac
done
'''
MOONLIGHT = '''#!/bin/sh
echo "$*" >> "$MOCK_STREAMS"
printf 'ready\n' > "$MOONLIGHT_READY_FILE"
exit 42
'''


class RecoveryIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.logs = self.root / 'logs'
        self.logs.mkdir()
        self.calls = self.root / 'calls'
        self.calls.touch()
        self.kodi = self.root / 'kodi'
        self.kodi.write_text('active\n')
        self.video = self.root / 'video_disable'
        self.video.write_text('0')
        self.fb0 = self.root / 'fb0_blank'
        self.fb0.write_text('1')
        self.streams = self.root / 'streams'
        for name, content in [('systemctl', SYSTEMCTL),
                              ('systemd-run', SYSTEMD_RUN),
                              ('moonlight', MOONLIGHT),
                              ('killall', '#!/bin/sh\nexit 0\n')]:
            path = self.root / name
            path.write_text(content)
            path.chmod(0o755)
        self.env = dict(os.environ, MOCK_KODI=str(self.kodi),
                        MOCK_CALLS=str(self.calls), MOCK_STREAMS=str(self.streams),
                        LUNA_LOG_DIR=str(self.logs),
                        MOONLIGHT_BINARY=str(self.root / 'moonlight'),
                        LUNA_SYSTEMCTL=str(self.root / 'systemctl'),
                        LUNA_SYSTEMD_RUN=str(self.root / 'systemd-run'),
                        LUNA_KILLALL=str(self.root / 'killall'),
                        LUNA_VIDEO_DISABLE=str(self.video),
                        LUNA_FB0_BLANK=str(self.fb0),
                        LUNA_RESUME_HOST='host-A')
        for key in ('LUNA_CPUFREQ_ROOT', 'LUNA_NET_CORE_ROOT', 'LUNA_TSYNC_ENABLE',
                    'LUNA_HDMI_ROOT', 'LUNA_FB1_BLANK',
                    'LUNA_VDEC_STATUS', 'LUNA_AML_LOW_LATENCY',
                    'LUNA_AML_SKIP_POLICY', 'LUNA_AML_REF_FLAGS'):
            self.env[key] = str(self.root / 'absent')

    def launch(self):
        return subprocess.run(['sh', str(SCRIPTS / 'launch.sh'), 'Desktop'],
                              env=self.env, text=True, capture_output=True, timeout=15)

    def old_session(self, boot, owner=None):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('1\n')
        # Fixture represents a launcher that crashed before native exec.
        Path(str(marker) + '.native-not-started').write_text('not-started\n')
        Path(str(marker) + '.boot-id').write_text(boot + '\n')
        if owner:
            Path(str(marker) + '.owner').write_text(owner + '\n')
        journals = [Path(str(marker) + '.' + kind + '-state')
                    for kind in ('governor', 'network', 'tsync', 'hdmi', 'aml')]
        for journal in journals:
            journal.touch()
        Path(str(marker) + '.journals').write_text(
            ''.join(str(journal) + '\n' for journal in journals))
        return marker

    def completed_session(self, native_started=True):
        """Fixture for a killed launcher after successful recovery/disarm."""
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        marker = self.old_session(boot, '%d|10' % dead_pid)
        marker.write_text('0\n')
        Path(str(marker) + '.kodi-initial').write_text('active\n')
        Path(str(marker) + '.unit').write_text(
            'luna-ng-session-%d.service\n' % dead_pid)
        Path(str(marker) + '.recovery.lock').touch()
        if native_started:
            Path(str(marker) + '.native-not-started').unlink()
            Path(str(marker) + '.native-launch-attempted').write_text('attempted\n')
            Path(str(marker) + '.native-exited').write_text('exited\n')
        return marker

    def test_unknown_initial_kodi_state_retires_unarmed_files(self):
        self.kodi.write_text('unknown\n')
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(self.streams.exists())
        self.assertEqual(self.video.read_text(), '0')
        self.assertNotIn('stop kodi.service', self.calls.read_text())
        snapshots = [p for p in self.logs.glob('.luna-session.*')
                     if p.is_file() and not p.name.endswith('.recovery.lock')]
        self.assertEqual(snapshots, [], snapshots)

    def test_initially_stopped_kodi_restores_native_state_without_starting_kodi(self):
        self.kodi.write_text('inactive\n')
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertEqual(self.kodi.read_text().strip(), 'inactive')
        self.assertNotIn('start kodi.service', self.calls.read_text())
        self.assertFalse(list(self.logs.glob('.luna-session.????????')))

    def test_initially_stopped_kodi_keeps_recovery_on_display_failure(self):
        self.kodi.write_text('inactive\n')
        self.env['LUNA_FB0_BLANK'] = str(self.root / 'missing-primary-fb')
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertEqual(self.kodi.read_text().strip(), 'inactive')
        self.assertNotIn('start kodi.service', self.calls.read_text())
        sessions = list(self.logs.glob('.luna-session.????????'))
        self.assertEqual(len(sessions), 1, sessions)
        self.assertEqual(sessions[0].read_text().strip(), '1')
        self.assertEqual(Path(str(sessions[0]) + '.kodi-initial').read_text().strip(),
                         'inactive')

    def test_aborted_handoff_never_writes_kodis_video_plane(self):
        self.env['FAIL_WATCHDOG'] = '1'
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(self.streams.exists())
        self.assertEqual(self.kodi.read_text().strip(), 'active')
        self.assertEqual(self.video.read_text(), '0')
        self.assertNotIn('stop kodi.service', self.calls.read_text())
        snapshots = [p for p in self.logs.glob('.luna-session.*')
                     if p.is_file() and not p.name.endswith('.recovery.lock')]
        self.assertEqual(snapshots, [], snapshots)

    def test_empty_legacy_session_marker_is_safely_retired(self):
        marker = self.logs / '.luna-session.old12345'
        marker.touch()
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())

    def test_empty_marker_with_metadata_remains_blocked(self):
        marker = self.logs / '.luna-session.old12345'
        marker.touch()
        Path(str(marker) + '.owner').write_text('123|4\n')
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())

    def test_torn_legacy_launch_manifest_cannot_delete_unrelated_file(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        Path(str(marker) + '.native-not-started').write_text('not-started\n')
        important = self.root / 'important'
        important.write_text('preserve')
        Path(str(marker) + '.launch-files').write_text(str(important) + '\n')
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())
        self.assertEqual(important.read_text(), 'preserve')

    def test_complete_unsafe_launch_manifest_remains_blocked(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        Path(str(marker) + '.native-not-started').write_text('not-started\n')
        important = self.root / 'important'
        important.write_text('preserve')
        Path(str(marker) + '.launch-files').write_text(
            '%s\n%s\n%s\n' % (self.root, important, self.root / 'invalid'))
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertEqual(important.read_text(), 'preserve')

    def test_crash_before_atomic_launch_manifest_rename_is_reclaimed(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        Path(str(marker) + '.native-not-started').write_text('not-started\n')
        pending = Path(str(marker) + '.launch-files.pending')
        pending.write_text('partial\n')
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(pending.exists())

    def test_old_unowned_recovery_lock_is_garbage_collected(self):
        lock = self.logs / '.luna-session.old12345.recovery.lock'
        lock.touch()
        past = time.time() - 4000
        os.utime(lock, (past, past))
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(lock.exists())

    def test_recent_and_locked_recovery_files_are_not_unlinked(self):
        recent = self.logs / '.luna-session.fresh123.recovery.lock'
        recent.touch()
        held = self.logs / '.luna-session.old12345.recovery.lock'
        held.touch()
        past = time.time() - 4000
        os.utime(held, (past, past))
        with held.open('r+') as stream:
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
            result = self.launch()
            self.assertEqual(result.returncode, 42, result.stderr)
            self.assertTrue(held.exists())
        self.assertTrue(recent.exists())

    def test_stale_unarmed_session_is_reclaimed_without_kodi_shutdown(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        Path(str(marker) + '.native-not-started').write_text('not-started\n')
        snapshot = self.root / '.luna-stream-stale.conf'
        receipt = self.root / '.luna-ack-stale.ack'
        snapshot.write_text('[Moonlight]')
        receipt.write_text('entered\n')
        Path(str(marker) + '.launch-files').write_text(
            '%s\n%s\n%s\n' % (self.root, snapshot, receipt))
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())
        self.assertFalse(snapshot.exists())
        self.assertFalse(receipt.exists())
        self.assertIn('stream -app Desktop', self.streams.read_text())

    def pre_arm_session(self, boot):
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        marker = self.old_session(boot, '%d|10' % dead_pid)
        marker.write_text('0\n')
        Path(str(marker) + '.kodi-initial').write_text('active\n')
        Path(str(marker) + '.unit').write_text('luna-ng-session-%d.service\n' % dead_pid)
        Path(str(marker) + '.ready').write_text('ready\n')
        Path(str(marker) + '.stream-ready').touch()
        Path(str(marker) + '.launch-files').write_text('%s\n\n\n' % self.root)
        # AML records its initial driver state before arming, without writing
        # sysfs. This nonempty snapshot is the case completed-retirement rejects.
        Path(str(marker) + '.aml-state').write_text('pre-arm driver snapshot\n')
        return marker

    def test_crash_after_initial_kodi_snapshot_before_arm_is_reclaimed(self):
        for boot in (Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                     'previous-boot'):
            with self.subTest(boot=boot):
                marker = self.pre_arm_session(boot)
                result = self.launch()
                self.assertEqual(result.returncode, 42, result.stderr)
                self.assertFalse(marker.exists())
                self.assertFalse(Path(str(marker) + '.kodi-initial').exists())
                self.assertFalse(Path(str(marker) + '.aml-state').exists())

    def test_pre_arm_retirement_rejects_later_handoff_or_cleanup_evidence(self):
        for suffix in ('startup', 'native-launch-attempted', 'recovery.lock'):
            with self.subTest(suffix=suffix):
                marker = self.pre_arm_session('previous-boot')
                evidence = Path(str(marker) + '.' + suffix)
                evidence.write_text('1\n')
                result = self.launch()
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertTrue(marker.exists())
                self.assertFalse(self.streams.exists())
                evidence.unlink()

    def test_collected_unit_with_dead_native_pid_recovers_without_exit_receipt(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        marker = self.old_session(boot, '%d|10' % dead_pid)
        Path(str(marker) + '.native-not-started').unlink()
        Path(str(marker) + '.native-launch-attempted').write_text('attempted\n')
        Path(str(marker) + '.moonlight-pid').write_text('%d|20|/test/moonlight\n' % (dead_pid + 1))
        self.kodi.write_text('inactive\n')
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())
        self.assertIn('start kodi.service', self.calls.read_text())

    def test_collected_unit_with_live_native_identity_cannot_recover(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        marker = self.old_session(boot, '%d|10' % dead_pid)
        Path(str(marker) + '.native-not-started').unlink()
        Path(str(marker) + '.native-launch-attempted').write_text('attempted\n')
        stat = Path('/proc/%d/stat' % os.getpid()).read_text()
        started = stat.rsplit(') ', 1)[1].split()[19]
        Path(str(marker) + '.moonlight-pid').write_text('%d|%s|/test/moonlight\n' % (os.getpid(), started))
        self.kodi.write_text('inactive\n')
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())
        self.assertNotIn('start kodi.service', self.calls.read_text())

    def test_crash_between_unarmed_marker_and_native_receipt_is_safe(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())

    def test_unarmed_missing_native_receipt_with_later_metadata_is_rejected(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        Path(str(marker) + '.owner').write_text('123|4\n')
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())

    def test_owner_only_pre_watchdog_metadata_is_reclaimed(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        marker = self.old_session(boot, '%d|10' % dead_pid)
        marker.write_text('0\n')
        # The owner receipt was committed, but .unit was never published.
        # launch-files and journals must already exist at this point.
        Path(str(marker) + '.launch-files').write_text(
            '%s\n\n\n' % self.root)
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())
        self.assertIn('stream -app Desktop', self.streams.read_text())

    def test_owner_only_live_process_cannot_be_retired(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        stat = Path('/proc/%d/stat' % os.getpid()).read_text()
        start = stat.rsplit(') ', 1)[1].split()[19]
        marker = self.old_session(boot, '%d|%s' % (os.getpid(), start))
        marker.write_text('0\n')
        Path(str(marker) + '.launch-files').write_text(
            '%s\n\n\n' % self.root)
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())

    def test_owner_only_incomplete_prior_metadata_is_rejected(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        marker = self.old_session(boot, 'bad-owner')
        marker.write_text('0\n')
        Path(str(marker) + '.launch-files').write_text(
            '%s\n\n\n' % self.root)
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())

    def test_owner_only_with_watchdog_ready_evidence_is_rejected(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        marker = self.old_session(boot, '123456789|10')
        marker.write_text('0\n')
        Path(str(marker) + '.launch-files').write_text(
            '%s\n\n\n' % self.root)
        Path(str(marker) + '.ready').touch()
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())

    def test_completed_zero_marker_is_reclaimed_after_verified_exit(self):
        marker = self.completed_session()
        config = self.root / '.luna-stream-completed.conf'
        config.write_text('[Moonlight]\n')
        Path(str(marker) + '.launch-files').write_text(
            '%s\n%s\n\n' % (self.root, config))
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())
        self.assertFalse(config.exists())
        self.assertFalse(Path(str(marker) + '.owner').exists())
        self.assertIn('stream -app Desktop', self.streams.read_text())

    def test_completed_zero_marker_without_native_attempt_is_reclaimed(self):
        marker = self.completed_session(native_started=False)
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())

    def test_completed_zero_marker_requires_native_exit_proof(self):
        marker = self.completed_session()
        Path(str(marker) + '.native-exited').unlink()
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())

    def test_completed_zero_marker_refuses_live_old_unit(self):
        marker = self.completed_session()
        self.env['MOCK_OLD_UNIT_STATE'] = 'active'
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())

    def test_completed_zero_marker_refuses_unrestored_journal(self):
        marker = self.completed_session()
        Path(str(marker) + '.network-state').write_text('pending\n')
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())

    def test_completed_zero_marker_respects_existing_recovery_lock(self):
        marker = self.completed_session()
        lock = Path(str(marker) + '.recovery.lock')
        with lock.open('r+') as stream:
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
            result = self.launch()
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertTrue(marker.exists())

    def test_completed_zero_marker_never_follows_untrusted_file_path(self):
        marker = self.completed_session()
        unrelated = self.root / 'important'
        unrelated.write_text('untouched')
        Path(str(marker) + '.launch-files').write_text(
            '%s\n%s\n\n' % (self.root, unrelated))
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertEqual(unrelated.read_text(), 'untouched')

    def test_partial_pre_watchdog_journals_are_reclaimed_without_manifest(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        Path(str(marker) + '.native-not-started').write_text('not-started\n')
        orphan_governor = Path(str(marker) + '.governor-state')
        orphan_network = Path(str(marker) + '.network-state')
        orphan_governor.write_text('')
        orphan_network.write_text('')
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())
        self.assertFalse(orphan_governor.exists())
        self.assertFalse(orphan_network.exists())

    def test_crash_during_unarmed_journal_manifest_write_is_reclaimed(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        Path(str(marker) + '.native-not-started').write_text('not-started\n')
        Path(str(marker) + '.journals').write_text(str(marker) + '.governor-state\n')
        Path(str(marker) + '.governor-state').touch()
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())
        self.assertFalse(Path(str(marker) + '.journals').exists())

    def test_crash_before_atomic_manifest_rename_clears_pending(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        Path(str(marker) + '.native-not-started').write_text('not-started\n')
        Path(str(marker) + '.journals.pending').write_text('incomplete')
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(Path(str(marker) + '.journals.pending').exists())

    def test_torn_manifest_with_watchdog_metadata_blocks_recovery(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        Path(str(marker) + '.native-not-started').write_text('not-started\n')
        Path(str(marker) + '.journals').write_text('torn\n')
        Path(str(marker) + '.ready').touch()
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())

    def test_unarmed_marker_with_unsafe_metadata_blocks_new_stream(self):
        marker = self.logs / '.luna-session.old12345'
        marker.write_text('0\n')
        Path(str(marker) + '.native-not-started').write_text('not-started\n')
        Path(str(marker) + '.moonlight-pid').write_text('123|4|/moonlight\n')
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())

    def test_old_boot_recovery_does_not_confuse_new_active_unit(self):
        marker = self.old_session('prior-kernel-boot')
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())
        self.assertIn('stream -app Desktop', self.streams.read_text())

    def test_old_boot_manifest_traversal_cannot_delete_unrelated_file(self):
        marker = self.old_session('prior-kernel-boot')
        external = self.root / 'unrelated-state'
        external.write_text('keep')
        manifest = Path(str(marker) + '.journals')
        entries = manifest.read_text().splitlines()
        entries[1] = str(external)
        manifest.write_text('\n'.join(entries) + '\n')
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('unsafe or incomplete', result.stderr)
        self.assertEqual(external.read_text(), 'keep')
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())

    def test_old_boot_manifest_rejects_embedded_traversal_and_extra_rows(self):
        marker = self.old_session('prior-kernel-boot')
        manifest = Path(str(marker) + '.journals')
        expected = manifest.read_text()
        with self.subTest('traversal'):
            entries = expected.splitlines()
            entries[0] = str(self.logs / '.luna-governor.12345678/../../outside')
            manifest.write_text('\n'.join(entries) + '\n')
            result = self.launch()
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertTrue(marker.exists())
        with self.subTest('extra rows'):
            manifest.write_text(expected + str(self.root / 'unrelated') + '\n')
            result = self.launch()
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertTrue(marker.exists())

    def test_same_boot_dead_owner_recovers_only_when_old_unit_inactive(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        marker = self.old_session(boot, '%d|10' % dead_pid)
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())
        self.assertIn('stream -app Desktop', self.streams.read_text())

    def test_missing_native_identity_and_exit_receipt_blocks_recovery(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        marker = self.old_session(boot, '%d|10' % dead_pid)
        Path(str(marker) + '.native-not-started').unlink()
        Path(str(marker) + '.native-launch-attempted').write_text('attempted\n')
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())
        self.assertIn('prior systemd unit or native process', result.stderr)

    def test_same_boot_native_exit_receipt_allows_stopped_unit(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        marker = self.old_session(boot, '%d|10' % dead_pid)
        Path(str(marker) + '.native-not-started').unlink()
        Path(str(marker) + '.native-launch-attempted').write_text('attempted\n')
        Path(str(marker) + '.native-exited').write_text('exited\n')
        result = self.launch()
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertFalse(marker.exists())

    def test_corrupt_native_receipts_never_unlock_same_boot_recovery(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        for invalid in ('', 'unverified', 'exited', 'not-started', 'not-started\n\n'):
            with self.subTest(value=invalid):
                marker = self.old_session(boot, '%d|10' % dead_pid)
                Path(str(marker) + '.native-not-started').write_text(invalid)
                result = self.launch()
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertTrue(marker.exists())
                self.assertFalse(self.streams.exists())

    def test_invalid_prior_session_arm_marker_blocks_new_stream(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        for invalid in ('', 'corrupt\n', '0\n\n', '1\n\n'):
            with self.subTest(value=invalid):
                marker = self.old_session(boot, '%d|10' % dead_pid)
                marker.write_text(invalid)
                result = self.launch()
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertTrue(marker.exists())
                self.assertFalse(self.streams.exists())
                self.assertEqual(self.video.read_text(), '0')

    def test_same_boot_active_or_deactivating_old_unit_blocks(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        for state in ('active', 'activating', 'deactivating', 'unknown'):
            with self.subTest(state=state):
                marker = self.old_session(boot, '%d|10' % dead_pid)
                self.env['MOCK_OLD_UNIT_STATE'] = state
                result = self.launch()
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertTrue(marker.exists())
                self.assertFalse(self.streams.exists())
                self.assertEqual(self.video.read_text(), '0')
                self.assertNotIn('stop kodi.service', self.calls.read_text())

    def test_same_boot_lingering_native_process_blocks(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        dead_pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 100
        marker = self.old_session(boot, '%d|10' % dead_pid)
        sleeper = subprocess.Popen(['sleep', '30'])
        try:
            stat = Path('/proc/%d/stat' % sleeper.pid).read_text()
            started = stat.rsplit(') ', 1)[1].split()[19]
            Path(str(marker) + '.moonlight-pid').write_text(
                '%d|%s|/usr/bin/sleep\n' % (sleeper.pid, started))
            result = self.launch()
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertTrue(marker.exists())
            self.assertFalse(self.streams.exists())
        finally:
            sleeper.terminate()
            sleeper.wait(timeout=5)

    def test_same_boot_live_owner_blocks_reconciliation(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        stat = Path('/proc/%d/stat' % os.getpid()).read_text()
        start = stat.rsplit(') ', 1)[1].split()[19]
        marker = self.old_session(boot, '%d|%s' % (os.getpid(), start))
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(marker.exists())
        self.assertFalse(self.streams.exists())
        self.assertEqual(self.video.read_text(), '0')
        self.assertEqual(self.kodi.read_text().strip(), 'active')


if __name__ == '__main__':
    unittest.main()
