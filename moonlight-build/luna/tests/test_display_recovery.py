"""Exercise the real display-recovery helper with missing required controls."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'resources/launchscripts/coreelec/display-recovery.sh'


class DisplayRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.journal = self.root / 'display-pending'
        self.video = self.root / 'video_disable'
        self.fb0 = self.root / 'fb0_blank'
        self.fb1 = self.root / 'fb1_blank'
        self.status = self.root / 'vdec_missing'

    def run_restore(self):
        return subprocess.run(['sh', str(SCRIPT), '--restore',
                               str(self.journal), str(self.video),
                               str(self.status), str(self.fb0), str(self.fb1)],
                              capture_output=True, text=True, timeout=8)

    def run_verify(self):
        return subprocess.run(['sh', str(SCRIPT), '--verify',
                               str(self.journal), str(self.fb0), str(self.fb1)],
                              capture_output=True, text=True, timeout=5)

    def test_all_controls_missing_never_disarms(self):
        result = self.run_restore()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.journal.read_text().strip(), 'unverified')

    def test_missing_primary_fb_never_disarms(self):
        self.video.write_text('0')
        result = self.run_restore()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(self.journal.exists())

    def test_missing_video_plane_never_disarms(self):
        self.fb0.write_text('1')
        result = self.run_restore()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(self.journal.exists())

    def test_required_nodes_are_written_and_verified(self):
        self.video.write_text('0')
        self.fb0.write_text('1')
        result = self.run_restore()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.video.read_text(), '1')
        self.assertEqual(self.fb0.read_text(), '0')
        self.assertFalse(self.journal.exists())

    def test_active_kodi_guard_refuses_video_and_framebuffer_writes(self):
        self.video.write_text('0')
        self.fb0.write_text('1')
        systemctl = self.root / 'systemctl'
        systemctl.write_text('#!/bin/sh\necho active\n')
        systemctl.chmod(0o755)
        env = dict(os.environ, LUNA_SYSTEMCTL=str(systemctl))
        result = subprocess.run(
            ['sh', str(SCRIPT), '--restore', str(self.journal),
             str(self.video), str(self.status), str(self.fb0), str(self.fb1),
             'kodi.service'], env=env, capture_output=True, text=True, timeout=8)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.video.read_text(), '0')
        self.assertEqual(self.fb0.read_text(), '1')
        self.assertTrue(self.journal.exists())

    def test_kodi_becomes_active_between_video_and_fb_writes(self):
        self.video.write_text('0')
        self.fb0.write_text('1')
        counter = self.root / 'counter'
        counter.write_text('0')
        systemctl = self.root / 'systemctl'
        systemctl.write_text(
            '#!/bin/sh\n'
            'n=$(cat "$MOCK_KODI_COUNTER")\n'
            'n=$((n + 1))\n'
            'echo "$n" > "$MOCK_KODI_COUNTER"\n'
            'if [ "$n" -eq 1 ]; then echo inactive; else echo active; fi\n')
        systemctl.chmod(0o755)
        env = dict(os.environ, LUNA_SYSTEMCTL=str(systemctl),
                   MOCK_KODI_COUNTER=str(counter))
        result = subprocess.run(
            ['sh', str(SCRIPT), '--restore', str(self.journal),
             str(self.video), str(self.status), str(self.fb0), str(self.fb1),
             'kodi.service'], env=env, capture_output=True, text=True, timeout=8)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.video.read_text(), '1')
        self.assertEqual(self.fb0.read_text(), '1')  # Kodi owns it now.
        self.assertEqual(self.journal.read_text().strip(), 'critical')

    def test_readonly_verification_refuses_unverified_operation(self):
        self.fb0.write_text('0')
        self.journal.write_text('unverified\n')
        self.assertEqual(self.run_verify().returncode, 1)
        self.journal.write_text('fb0\n')
        self.assertEqual(self.run_verify().returncode, 0)
        self.fb0.unlink()
        self.assertEqual(self.run_verify().returncode, 1)


if __name__ == '__main__':
    unittest.main()
