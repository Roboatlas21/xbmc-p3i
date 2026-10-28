"""Test native pairing failures against a host whose /unpair returns 404.

Requires a C compiler, initialized moonlight-common-c, and development headers
and libraries for curl, OpenSSL, expat, and uuid. Runs in about 17 seconds.
"""
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


class PairingErrorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        main = root / 'pair.c'
        main.write_text('''
#include "client.h"
#include "http.h"
#include "errors.h"
#include <stdio.h>
int main(int argc, char **argv) {
    SERVER_DATA server = {0};
    server.serverInfo.address = "127.0.0.1";
    server.httpPort = atoi(argv[1]);
    server.serverMajorVersion = 7;
    if (http_init(".", 2) != GS_OK) return 2;
    int result = gs_pair(&server, "1234");
    printf("PAIR_ERROR: %s\\n", gs_error ? gs_error : "none");
    return result == GS_OK ? 0 : 1;
}
''')
        cls.executable = root / 'pair'
        common = os.environ.get('MOONLIGHT_COMMON_INCLUDE',
                                str(ROOT / 'third_party/moonlight-common-c/src'))
        command = shlex.split(os.environ.get('CC', 'cc'))
        command += shlex.split(os.environ.get('CFLAGS', ''))
        command += ['-ffunction-sections', '-fdata-sections',
                    '-Wno-deprecated-declarations', '-I' + common,
                    '-I' + str(ROOT / 'libgamestream'), str(main)]
        command += [str(ROOT / 'libgamestream' / name)
                    for name in ('client.c', 'http.c', 'xml.c')]
        command += ['-Wl,--gc-sections']
        command += shlex.split(os.environ.get('LDFLAGS', ''))
        command += ['-lcurl', '-lssl', '-lcrypto', '-lexpat', '-l:libuuid.so.1',
                    '-o', str(cls.executable)]
        subprocess.run(command, check=True)

    def run_pair(self, mode):
        requests = []

        class Host(BaseHTTPRequestHandler):
            def do_GET(self):
                path = self.path.split('?')[0]
                requests.append(path)
                if path == '/unpair':
                    status, body = 404, b'<root status_code="404"/>'
                elif mode == 'disconnect':
                    self.close_connection = True
                    return
                elif mode == 'http_error':
                    status, body = 403, b'Forbidden'
                else:
                    # The first pairing request must outlive the old 15s limit.
                    time.sleep(16)
                    status = 200
                    body = (b'<root status_code="400" '
                            b'status_message="Out of order call to getservercert">'
                            b'<paired>0</paired></root>')
                self.send_response(status)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_args):
                pass

        with ThreadingHTTPServer(('127.0.0.1', 0), Host) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                result = subprocess.run(
                    [str(self.executable), str(server.server_port)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, timeout=25)
            finally:
                server.shutdown()
                thread.join()
        self.assertEqual(requests, ['/pair', '/unpair'])
        self.assertEqual(result.returncode, 1, result.stdout)
        return result.stdout

    def test_delayed_server_error_survives_cleanup(self):
        output = self.run_pair('xml_error')
        self.assertIn('PAIR_ERROR: Out of order call to getservercert', output)

    def test_transport_error_survives_cleanup(self):
        output = self.run_pair('disconnect')
        self.assertIn('PAIR_ERROR: Server returned nothing', output)
        self.assertIn('curl 52, HTTP 0', output)

    def test_http_statuses_and_request_order_are_visible(self):
        output = self.run_pair('http_error')
        self.assertIn('curl 22, HTTP 403', output)
        self.assertIn('curl 22, HTTP 404', output)
        self.assertLess(output.index('Request http://'),
                        output.index('HTTP request failed:'))


if __name__ == '__main__':
    unittest.main()
