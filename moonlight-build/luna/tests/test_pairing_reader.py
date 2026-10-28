import os
import queue
import signal
import sys
import threading
import time
import types
import unittest
from unittest import mock


xbmc = sys.modules.get('xbmc')
if xbmc is None:
    xbmc = types.ModuleType('xbmc')
    sys.modules['xbmc'] = xbmc
xbmc.LOGINFO = 1
xbmc.log = lambda *args, **kwargs: None

xbmcgui = sys.modules.get('xbmcgui')
if xbmcgui is None:
    xbmcgui = types.ModuleType('xbmcgui')
    sys.modules['xbmcgui'] = xbmcgui

from resources.lib.util.moonlighthelper import MoonlightHelper


class PairingOutputReaderTests(unittest.TestCase):
    def test_buffered_connection_and_pin_burst_delivers_both_lines(self):
        read_fd, write_fd = os.pipe()
        stream = os.fdopen(read_fd, 'r', encoding='utf-8', buffering=1)
        output_queue = queue.Queue()

        burst = (
            'Connecting to 10.0.0.59...\n'
            'Please enter the following PIN on the target PC: 1234\n'
        ).encode('utf-8')

        # One write reproduces the case where TextIOWrapper can buffer the
        # connection line and PIN line together before returning readline().
        os.write(write_fd, burst)
        os.close(write_fd)

        reader = threading.Thread(
            target=MoonlightHelper._read_pairing_output,
            args=(stream, output_queue),
            daemon=True)

        helper = MoonlightHelper.__new__(MoonlightHelper)
        helper.pin_received = False
        helper.paired_address = None
        helper.pairing_messages = ''

        class Dialog:
            def __init__(self):
                self.updates = []

            def update(self, percent, message):
                self.updates.append((percent, message))

        dialog = Dialog()
        started = time.monotonic()
        pre_pin_timeout = started + 20
        events = []

        try:
            reader.start()

            while True:
                event, value = output_queue.get(timeout=1)
                events.append((event, value))

                if event == 'line':
                    helper._pair_line(value, dialog)
                elif event == 'error':
                    self.fail('pairing reader raised %r' % (value,))
                elif event == 'eof':
                    break

            reader.join(timeout=1)

            self.assertFalse(reader.is_alive())
            self.assertEqual(
                [value for event, value in events if event == 'line'],
                [
                    'Connecting to 10.0.0.59...\n',
                    'Please enter the following PIN on the target PC: 1234\n',
                ])
            self.assertEqual(events[-1], ('eof', None))
            self.assertEqual(helper.paired_address, '10.0.0.59')
            self.assertTrue(helper.pin_received)
            self.assertLess(time.monotonic(), pre_pin_timeout)
            self.assertEqual(
                dialog.updates,
                [(50, 'Please enter the following PIN on the target PC: 1234')])
        finally:
            stream.close()

    def test_cancel_terminates_moonlight_and_reader_thread_exits(self):
        read_fd, write_fd = os.pipe()
        stream = os.fdopen(read_fd, 'r', encoding='utf-8', buffering=1)
        writer = os.fdopen(write_fd, 'w', encoding='utf-8', buffering=1)
        writer.write('Connecting to 10.0.0.59...\n')
        writer.flush()

        class Process:
            pid = 4242

            def __init__(self):
                self.stdout = stream
                self.returncode = None
                self.signal = None

            def poll(self):
                return self.returncode

            def wait(self, timeout=None):
                if self.returncode is None:
                    raise AssertionError(
                        'wait() called before the fake process was terminated')
                return self.returncode

            def terminate(self):
                self._finish(signal.SIGTERM)

            def kill(self):
                self._finish(signal.SIGKILL)

            def _finish(self, sig):
                self.signal = sig
                self.returncode = -int(sig)
                if not writer.closed:
                    writer.close()

        class Dialog:
            def __init__(self):
                self.closed = False

            def create(self, *args):
                pass

            def iscanceled(self):
                return True

            def update(self, *args):
                pass

            def close(self):
                self.closed = True

        process = Process()
        dialog = Dialog()
        config_helper = types.SimpleNamespace(
            configure=lambda: None,
            binary_path='/tmp/luna/bin/',
            addon_path='/tmp/luna/',
            config={'host_addr': '10.0.0.59'})
        plugin = types.SimpleNamespace(getLocalizedString=lambda _ident: '')
        helper = MoonlightHelper(plugin, config_helper, types.SimpleNamespace())

        def terminate_group(_pgid, sig):
            process._finish(sig)

        try:
            with mock.patch(
                    'resources.lib.util.hostapi.Host.paired',
                    return_value=False), \
                 mock.patch(
                    'resources.lib.util.moonlighthelper.subprocess.Popen',
                    return_value=process), \
                 mock.patch.object(
                    xbmcgui, 'DialogProgress',
                    return_value=dialog, create=True), \
                 mock.patch(
                    'resources.lib.util.moonlighthelper.os.getpgid',
                    return_value=process.pid), \
                 mock.patch(
                    'resources.lib.util.moonlighthelper.os.killpg',
                    side_effect=terminate_group) as killpg:
                self.assertFalse(helper.pair())

            killpg.assert_called_once_with(process.pid, signal.SIGTERM)
            self.assertEqual(process.signal, signal.SIGTERM)
            self.assertIsNotNone(process.poll())
            self.assertTrue(dialog.closed)
            self.assertFalse(any(
                thread.name == 'LunaPairingReader' and thread.is_alive()
                for thread in threading.enumerate()))
        finally:
            if not writer.closed:
                writer.close()
            if not stream.closed:
                stream.close()


if __name__ == '__main__':
    unittest.main()
