"""A Luna snapshot must supersede—not inherit from—shared moonlight.conf."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SessionConfigPrecedenceTests(unittest.TestCase):
    def test_exclusive_snapshot_selected_before_default_configuration(self):
        source = (ROOT / "src/config.c").read_text()
        start = source.index('const char *session_config = getenv("LUNA_STREAM_CONFIG")')
        default = source.index('char *config_file = get_path("moonlight.conf"', start)
        explicit = source.index("getopt_long_only(", default)
        self.assertLess(start, default)
        self.assertLess(default, explicit)
        self.assertIn('if (!use_session_only)', source[start:explicit])
        self.assertIn('strcmp(argv[i + 1], session_config) == 0',
                      source[start:default])
        self.assertIn('config_file_parse(value, config)', source)

    def test_snapshot_skips_mutable_per_host_config(self):
        main = (ROOT / "src/main.c").read_text()
        self.assertIn('const char *session_snapshot = getenv("LUNA_STREAM_CONFIG")',
                      main)
        self.assertIn('if (!(session_snapshot && *session_snapshot) &&',
                      main)
        self.assertLess(main.index('if (!(session_snapshot && *session_snapshot) &&'),
                        main.index('config_file_parse(host_config_file, &config)'))


if __name__ == '__main__':
    unittest.main()
