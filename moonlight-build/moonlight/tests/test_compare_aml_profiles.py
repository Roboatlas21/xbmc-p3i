import tempfile
import unittest
from pathlib import Path

from scripts.compare_aml_profiles import read_latest, compare, format_value


class CompareAmlProfilesTests(unittest.TestCase):
    def test_extract_latest_session_and_skip_bad_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "luna.log"
            path.write_text(
                "hello\nAML_PROFILE_V1 frames=0 submit_mean_us=100\n"
                "[Kodi] AML_PROFILE_V1 frames=120 submit_mean_us=4000 eagain=12\n"
                "AML_PROFILE_V1 frames=60 submit_mean_us=700 eagain=3\n")
            parsed = read_latest(path)
            self.assertEqual(parsed["frames"], 60)
            self.assertEqual(parsed["submit_mean_us"], 700)

    def test_formatter_does_not_call_the_metric_end_to_end_latency(self):
        a = dict(frames=100, submit_mean_us=500, submit_p95_bucket_us=0,
                 eagain=3, failed=0)
        result = compare(a, a)
        self.assertIn("V4L2 dequeue cycles", result)
        self.assertIn("DO NOT measure end-to-end latency", result)
        self.assertEqual(format_value("submit_p95_bucket_us", a), ">132000")

    def test_empty_logs_fail_explicitly(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "empty.log"
            path.write_text("nothing")
            with self.assertRaisesRegex(ValueError, "No AML_PROFILE_V1"):
                read_latest(path)


if __name__ == "__main__":
    unittest.main()
