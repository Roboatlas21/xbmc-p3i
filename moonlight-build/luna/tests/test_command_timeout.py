"""Exercise real subprocess deadlines and cleanup without a system timeout tool."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

RUNNER = Path(__file__).resolve().parents[1] / 'resources/launchscripts/coreelec/command-timeout.py'


class CommandTimeoutTests(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run([sys.executable, str(RUNNER), *args],
                              capture_output=True, text=True, timeout=5)

    def test_arguments_output_and_exit_status_are_preserved(self):
        result = self.invoke('2', sys.executable, '-c',
                             'import sys; print(sys.argv[1]); sys.exit(23)',
                             'literal $(no-shell) ; &')
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertEqual(result.stdout, 'literal $(no-shell) ; &\n')
        self.assertEqual(self.invoke('2', '/no-such-luna-command').returncode, 127)

    def test_invalid_deadlines_never_execute_command(self):
        for value in ('nan', 'inf', '-1', '0'):
            with self.subTest(value=value):
                result = self.invoke(value, sys.executable, '-c', 'print("ran")')
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, '')

    def assert_stopped(self, pid):
        stat = Path('/proc/%d/stat' % pid)
        if stat.exists():
            self.assertIn(stat.read_text().rsplit(') ', 1)[1].split()[0], ('Z', 'X'))

    def test_deadline_kills_descendant_even_when_parent_exits_on_term(self):
        with tempfile.TemporaryDirectory() as directory:
            pidfile = Path(directory) / 'pid'
            code = ('import subprocess, sys, time; from pathlib import Path; '
                    'p=subprocess.Popen([sys.executable,"-c",'
                    '"import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(60)"]); '
                    'Path(sys.argv[1]).write_text(str(p.pid)); time.sleep(60)')
            started = time.monotonic()
            result = self.invoke('-k', '0.1', '0.5', sys.executable, '-c', code, str(pidfile))
            self.assertEqual(result.returncode, 124, result.stderr)
            self.assertLess(time.monotonic() - started, 3)
            self.assert_stopped(int(pidfile.read_text()))

    def test_outer_cancellation_does_not_orphan_nested_command_group(self):
        with tempfile.TemporaryDirectory() as directory:
            pidfile = Path(directory) / 'pid'
            code = ('import os,sys,time,signal; from pathlib import Path; '
                    'signal.signal(signal.SIGTERM,signal.SIG_IGN); '
                    'Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(60)')
            command = [sys.executable, str(RUNNER), '60', sys.executable, str(RUNNER),
                       '60', sys.executable, '-c', code, str(pidfile)]
            proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 3
                while not pidfile.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(pidfile.exists())
                proc.send_signal(signal.SIGTERM)
                proc.communicate(timeout=3)
                self.assertEqual(proc.returncode, 128 + signal.SIGTERM)
                self.assert_stopped(int(pidfile.read_text()))
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate(timeout=3)
                if pidfile.exists():
                    try:
                        os.kill(int(pidfile.read_text()), signal.SIGKILL)
                    except ProcessLookupError:
                        pass
