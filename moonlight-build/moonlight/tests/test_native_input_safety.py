"""Regression guards for native input bounds and credential path parsing."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeInputSafetyTests(unittest.TestCase):
    def test_all_four_linux_hat_axes_fit_state_array(self):
        code = (ROOT / 'src/input/evdev.c').read_text()
        match = re.search(r'\bhats_state\s*\[\s*(\d+)\s*\]\s*\[\s*(\d+)\s*\]', code)
        self.assertIsNotNone(match)
        self.assertGreaterEqual(int(match.group(1)), 4)
        self.assertEqual(int(match.group(2)), 2)
        for suffix in ('0X', '0Y', '1X', '1Y', '2X', '2Y', '3X', '3Y'):
            self.assertIn('ABS_HAT' + suffix + ':', code)

    def test_mapping_handlers_use_bounded_axis_indices(self):
        source = (ROOT / 'src/input/evdev.c').read_text()
        mapping = source.split('static bool evdev_handle_mapping_event(', 1)[1].split(
            'static void evdev_drain(', 1)[0]
        self.assertIn('if (ev->code > KEY_MAX)', mapping)
        self.assertIn('if (ev->code > ABS_MAX)', mapping)
        self.assertIn('const int mapped = dev->abs_map[ev->code]', mapping)
        self.assertIn('evdev_init_parms(dev, &parms, mapped)', mapping)
        self.assertIn('ev->code >= ABS_HAT0X', mapping)
        self.assertNotIn('evdev_init_parms(dev, &parms, ev->code)', mapping)

    def test_keydir_validates_before_copy(self):
        code = (ROOT / 'src/config.c').read_text()
        section = code.split("case 'r':", 1)[1].split('break;', 1)[0]
        self.assertIn('sizeof(config->key_dir)', section)
        self.assertNotIn('strcpy(', section)
        self.assertIn('exit(EXIT_FAILURE)', section)


if __name__ == '__main__':
    unittest.main()
