"""Exercise real libcurl against delayed PIN responses (about 33 seconds).

Run with python3 tests/test_http_pairing_timeout.py; requires cc and libcurl-dev.
"""
import ctypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Reply(BaseHTTPRequestHandler):
    def do_GET(self):
        time.sleep(float(self.path.strip('/')))
        body = b'<root><paired>1</paired></root>'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when the client timeout expires first.

    def log_message(self, *_args):
        pass


class HttpData(ctypes.Structure):
    _fields_ = [('memory', ctypes.c_char_p), ('size', ctypes.c_size_t)]


class PairingTimeoutTests(unittest.TestCase):
    def test_pin_wait_and_subsequent_request_deadlines(self):
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            (temp / 'error.c').write_text('const char *gs_error;\n')
            library = temp / 'http-test.so'
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                str(ROOT / 'libgamestream/http.c'), str(temp / 'error.c'),
                '-lcurl', '-o', str(library),
            ], check=True)
            http = ctypes.CDLL(str(library))
            pointer = ctypes.POINTER(HttpData)
            http.http_init.argtypes = [ctypes.c_char_p, ctypes.c_int]
            http.http_create_data.restype = pointer
            http.http_request.argtypes = [ctypes.c_char_p, pointer]
            http.http_request_with_timeout.argtypes = [
                ctypes.c_char_p, pointer, ctypes.c_long]
            http.http_free_data.argtypes = [pointer]
            server = ThreadingHTTPServer(('127.0.0.1', 0), Reply)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            self.assertEqual(http.http_init(temporary.encode(), 0), 0)
            data = http.http_create_data()
            self.assertTrue(data)
            base = 'http://127.0.0.1:%d/' % server.server_port

            def request(delay, timeout=None):
                url = (base + str(delay)).encode()
                start = time.monotonic()
                result = (http.http_request(url, data) if timeout is None else
                          http.http_request_with_timeout(url, data, timeout))
                return result, time.monotonic() - start

            try:
                # Submitting a PIN after 15 seconds must still succeed.
                self.assertEqual(request(16, 65000)[0], 0)
                self.assertIn(b'<paired>1</paired>', data.contents.memory)
                # A later hung request must not inherit the longer PIN wait.
                result, elapsed = request(16)
                self.assertEqual(result, -1)
                self.assertGreater(elapsed, 14)
                self.assertLess(elapsed, 16)
                # A failed custom request must not poison the next deadline.
                self.assertEqual(request(1, 250)[0], -1)
                self.assertEqual(request(1)[0], 0)
                self.assertEqual(request(0)[0], 0)
            finally:
                http.http_free_data(data)
                http.http_cleanup()
                server.shutdown()
                server.server_close()
                thread.join()


if __name__ == '__main__':
    unittest.main()
