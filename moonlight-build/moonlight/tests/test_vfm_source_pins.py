"""Early cross-repository pin checks must fail before a native build starts."""
import importlib.util
from pathlib import Path
import unittest

FILE = Path(__file__).resolve().parents[1] / "packaging/coreelec-ng/vfm/check-source-pins.py"
spec = importlib.util.spec_from_file_location("vfm_source_pins", FILE)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class SourcePins(unittest.TestCase):
    def setUp(self):
        self.revision = "a" * 40
        self.pins = {"Luna": self.revision}
        self.lock = {"schema": "luna-public-source-snapshot-v1",
                     "sources": {
                         "luna": {"repository": "Roboatlas21/Luna",
                                  "revision": self.revision},
                         "moonlight": {"repository": "Roboatlas21/moonlight-embedded",
                                       "revision": "b" * 40}}}

    def test_matching_snapshot(self):
        self.assertEqual(checker.check(self.pins, self.lock), self.revision)

    def test_stale_luna_pin_rejected(self):
        self.lock["sources"]["luna"]["revision"] = "c" * 40
        with self.assertRaisesRegex(ValueError, "packaging source mismatch"):
            checker.check(self.pins, self.lock)

    def test_wrong_repository_rejected(self):
        self.lock["sources"]["luna"]["repository"] = "other/repo"
        with self.assertRaisesRegex(ValueError, "Unexpected snapshot repository"):
            checker.check(self.pins, self.lock)

    def test_malformed_lock_rejected(self):
        self.lock["schema"] = "invalid"
        with self.assertRaisesRegex(ValueError, "Unexpected public snapshot schema"):
            checker.check(self.pins, self.lock)


if __name__ == '__main__':
    unittest.main()
