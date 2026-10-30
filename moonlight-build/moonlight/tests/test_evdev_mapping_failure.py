"""Ensure mapping failures exit nonzero rather than printing incomplete mappings."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MappingFailureTests(unittest.TestCase):
    def test_map_action_propagates_native_status(self):
        main = (ROOT / 'src/main.c').read_text()
        self.assertIn('return evdev_map(config.inputs[0]) ? EXIT_SUCCESS : EXIT_FAILURE;', main)
        header = (ROOT / 'src/input/evdev.h').read_text()
        self.assertIn('bool evdev_map(char* device);', header)

    def test_each_mapping_prompt_checks_poll_failure(self):
        text = (ROOT / 'src/input/evdev.c').read_text()
        for name in ('key', 'abs', 'hatkey', 'abskey'):
            match = re.search(r'static bool evdev_map_' + name +
                              r'\([^)]*\) \{(.*?)\n\}', text, re.DOTALL)
            self.assertIsNotNone(match, name)
            self.assertIn('if (loop_main() != LOOP_OK)', match.group(1))
            self.assertIn('return false;', match.group(1))
        self.assertIn('if (numDevices == 0)', text)
        self.assertGreaterEqual(text.count('MAP_STEP(evdev_map_'), 15)


if __name__ == '__main__':
    unittest.main()
