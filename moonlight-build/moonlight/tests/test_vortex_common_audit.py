import unittest
from scripts.audit_vortex_common import parse_symbols


class VortexCommonAuditTests(unittest.TestCase):
    def test_symbol_parser(self):
        text = """
  12: 0000000000018280     4 FUNC    GLOBAL DEFAULT   11 enterLowLatencyMode
  13: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND foo@GLIBC_2.17
  15: 0000000000012a70   104 FUNC    GLOBAL DEFAULT   11 sendInputPacketOnControlStream
"""
        parsed = parse_symbols(text)
        self.assertEqual(parsed["enterLowLatencyMode"], (0x18280, 4))
        self.assertEqual(parsed["sendInputPacketOnControlStream"], (0x12a70, 104))
        self.assertNotIn("foo", parsed)


if __name__ == "__main__":
    unittest.main()
