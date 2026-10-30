"""Verify AML validates decode units before diagnostics or tracing uses them."""
from pathlib import Path
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src/video/aml.c")


class AmlGuardOrderTests(unittest.TestCase):
    def test_invalid_unit_rejected_before_any_member_access(self):
        code = SOURCE.read_text()
        body = code.split("int aml_submit_decode_unit(PDECODE_UNIT decodeUnit) {", 1)[1]
        first_validation = body.index("if (!aml_decode_unit_valid(decodeUnit))")
        first_member = body.index("decodeUnit->")
        first_buffer = body.index("ensure_buf_size(&pkt_buf")
        self.assertLess(first_validation, first_member)
        self.assertLess(first_validation, first_buffer)
        self.assertEqual(body.count("if (!aml_decode_unit_valid(decodeUnit))"), 1)


if __name__ == "__main__":
    unittest.main()
