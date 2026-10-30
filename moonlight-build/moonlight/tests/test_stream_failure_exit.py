"""Check remote and VFM asynchronous errors cannot return cleanly."""
import pathlib
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]

class StreamFailureExitTests(unittest.TestCase):
    def test_remote_graceful_distinguished_from_failure(self):
        src = (ROOT / "src/connection.c").read_text()
        self.assertIn("errorCode == ML_ERROR_GRACEFUL_TERMINATION", src)
        self.assertIn("loop_request_error_stop();", src)
        self.assertIn("loop_request_stop();", src)

    def test_sdl_exits_nonzero_after_remote_failure(self):
        src = (ROOT / "src/main.c").read_text()
        self.assertIn('input_loop_status = loop_stream_failed() ? LOOP_ERROR : LOOP_OK;', src)
        self.assertIn('if (loop_stream_failed())', src)
        callback = (ROOT / "src/connection.c").read_text()
        self.assertIn('else\n    loop_request_error_stop();', callback)

    def test_vfm_failure_requests_error_stop(self):
        src = (ROOT / "src/platform.c").read_text()
        self.assertIn("init(loop_request_error_stop)", src)

    def test_loop_checks_fatal_reason_before_reporting_normal_exit(self):
        src = (ROOT / "src/loop.c").read_text()
        self.assertIn("return loop_exit_result();", src)
        self.assertIn("__atomic_store_n(&loopError, true", src)
        self.assertIn("return LOOP_ERROR;", src)

if __name__ == "__main__":
    unittest.main()
