"""Check the native config lookup failure and final-segment guards."""
from pathlib import Path
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "src/config.c").read_text()

class ConfigLookupSafetyTests(unittest.TestCase):
    def test_malloc_and_passwd_checked_before_use(self):
        find = SOURCE.split("char* get_path(", 1)[1].split("static void parse_argument(", 1)[0]
        self.assertIn("if (!data_dirs)", find)
        self.assertIn("if (!pw || !pw->pw_dir", find)
        self.assertIn("if (!end)", find)
        self.assertLess(find.index("if (!data_dirs)"), find.index("sprintf(data_dirs"))

    def test_stream_numbers_use_checked_parser_before_auto_bitrate(self):
        for option in ('width', 'height', 'fps', 'packetsize', 'bitrate'):
            self.assertIn('parse_stream_number("%s", value,' % option, SOURCE)
        self.assertNotIn('config->stream.width = atoi(value)', SOURCE)
        self.assertNotIn('config->stream.height = atoi(value)', SOURCE)
        self.assertNotIn('config->stream.fps = atoi(value)', SOURCE)
        validated = SOURCE.index('if (!moonlight_stream_numbers_valid(')
        pixel_count = SOURCE.index('const uint64_t pixels = moonlight_stream_pixel_count(')
        self.assertLess(validated, pixel_count)
        self.assertNotIn('config->stream.width * config->stream.height <=', SOURCE)

    def test_explicit_pin_port_and_rotation_are_bounded(self):
        self.assertIn('parse_stream_number("rotation", value, 0, 270)', SOURCE)
        self.assertIn('parse_stream_number("pairing PIN", value, 1, 9999)', SOURCE)
        self.assertIn('parse_stream_number("host port", value, 1, 65535)', SOURCE)
        self.assertIn('config->rotate != 90', SOURCE)
        self.assertIn('config->rotate != 180', SOURCE)
        self.assertIn('config->rotate != 270', SOURCE)
        self.assertNotIn('config->rotate = atoi(value)', SOURCE)
        self.assertNotIn('config->pin = atoi(value)', SOURCE)
        self.assertNotIn('config->port = atoi(value)', SOURCE)

    def test_credentials_require_known_home(self):
        self.assertIn("Cannot determine credential directory", SOURCE)
        self.assertIn("exit(EXIT_FAILURE);", SOURCE)

if __name__ == "__main__":
    unittest.main()
