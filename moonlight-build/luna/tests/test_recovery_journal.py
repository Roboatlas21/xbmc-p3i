"""Recovery journals must not disappear after an interrupted final write."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'resources/launchscripts/coreelec/recovery-journal.sh'


class RecoveryJournalTests(unittest.TestCase):
    def run_restore(self, journal):
        return subprocess.run(
            ['sh', '-c', '. "$1"; restore_tuning_journal "$2" network',
             'journal-test', str(SCRIPT), str(journal)],
            env=dict(os.environ, LUNA_NET_CORE_ROOT=str(journal.parent)),
            capture_output=True, text=True, timeout=5)

    def test_successful_restore_retires_journal(self):
        with tempfile.TemporaryDirectory() as tmp:
            node = Path(tmp, 'rmem_max')
            node.write_text('8')
            journal = Path(tmp, 'journal')
            journal.write_text('%s|3|8\n' % node)
            result = self.run_restore(journal)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(node.read_text(), '3')
            self.assertFalse(journal.exists())

    def test_partial_final_record_is_retained_and_can_be_retried(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = Path(tmp, 'rmem_max')
            second = Path(tmp, 'rmem_default')
            first.write_text('8')
            second.write_text('9')
            journal = Path(tmp, 'journal')
            complete = '%s|3|8\n%s|7|9' % (first, second)
            journal.write_text(complete)
            failed = self.run_restore(journal)
            self.assertNotEqual(failed.returncode, 0)
            self.assertTrue(journal.exists())
            self.assertEqual(second.read_text(), '9')
            self.assertIn('truncated', failed.stderr)
            journal.write_text(complete + '\n')
            recovered = self.run_restore(journal)
            self.assertEqual(recovered.returncode, 0, recovered.stderr)
            self.assertEqual(first.read_text(), '3')
            self.assertEqual(second.read_text(), '7')
            self.assertFalse(journal.exists())

    def test_journal_cannot_redirect_restore_outside_sysctl_allowlist(self):
        with tempfile.TemporaryDirectory() as tmp:
            trusted = Path(tmp, 'rmem_max')
            unrelated = Path(tmp, 'unrelated')
            trusted.write_text('8')
            unrelated.write_text('8')
            journal = Path(tmp, 'journal')
            journal.write_text(str(unrelated) + '|3|8\n')
            result = self.run_restore(journal)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('unsafe tuning recovery', result.stderr)
            self.assertEqual(unrelated.read_text(), '8')
            self.assertTrue(journal.exists())


if __name__ == '__main__':
    unittest.main()
