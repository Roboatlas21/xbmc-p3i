"""Exercise descriptor ownership and bounded contention with real flock."""
import fcntl
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

HELPER = Path(__file__).resolve().parents[1] / 'resources/launchscripts/coreelec/kodi-state.sh'


class RecoveryLockTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lock = Path(self.tmp.name) / 'recovery.lock'
        self.lock.touch()

    def acquire(self, seconds):
        # Keep the parent shell alive after the helper exits: its fd must
        # retain the lock. PATH can select CoreELEC-compatible BusyBox flock.
        return subprocess.Popen(
            ['sh', '-c', '. "$1"; exec 9>"$2"; '
             'luna_lock_with_timeout "$3" 9 || exit 1; '
             'echo acquired; read -r release', 'test', str(HELPER),
             str(self.lock), str(seconds)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True)

    def test_free_lock_remains_owned_by_calling_shell(self):
        owner = self.acquire(1)
        try:
            self.assertEqual(owner.stdout.readline(), 'acquired\n')
            with self.lock.open() as probe:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
        finally:
            owner.communicate('release\n', timeout=5)
        self.assertEqual(owner.returncode, 0)
        with self.lock.open() as probe:
            fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_released_lock_is_acquired_during_bounded_wait(self):
        with self.lock.open() as holder:
            fcntl.flock(holder, fcntl.LOCK_EX)
            waiter = self.acquire(3)
            time.sleep(0.2)
            self.assertIsNone(waiter.poll())
            fcntl.flock(holder, fcntl.LOCK_UN)
            out, err = waiter.communicate('release\n', timeout=5)
        self.assertEqual(waiter.returncode, 0, err)
        self.assertEqual(out, 'acquired\n')

    def test_timeout_preserves_existing_owner_and_lock_inode(self):
        inode = self.lock.stat().st_ino
        with self.lock.open() as holder:
            fcntl.flock(holder, fcntl.LOCK_EX)
            started = time.monotonic()
            waiter = self.acquire(1)
            out, err = waiter.communicate(timeout=5)
            elapsed = time.monotonic() - started
            self.assertEqual(waiter.returncode, 1, err)
            self.assertEqual(out, '')
            self.assertGreaterEqual(elapsed, 0.9)
            self.assertLess(elapsed, 4)
            self.assertEqual(self.lock.stat().st_ino, inode)
            with self.lock.open() as probe:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)


if __name__ == '__main__':
    unittest.main()
