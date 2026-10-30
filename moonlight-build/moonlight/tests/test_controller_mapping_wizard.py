"""Wizard must never mutate a previous axis inversion during hat/button mapping."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/input/evdev.c").read_text()

class WizardTests(unittest.TestCase):
    def test_mapping_defaults_match_parser(self):
        section = SOURCE.split("bool evdev_map(char* device) {", 1)[1]
        self.assertIn("memset(&map.abs_leftx, 0xff,", section)
        self.assertIn("offsetof(struct mapping, next)", section)

    def test_hat_does_not_touch_previous_reverse_pointer(self):
        match = re.search(r"static bool evdev_map_hatkey\(.*?\n\}", SOURCE, re.S)
        self.assertIsNotNone(match)
        self.assertIn("currentReverse = NULL;", match.group(0))
        self.assertNotIn("*currentReverse =", match.group(0))

    def test_unipolar_trigger_neutral_and_hat_release_are_distinct(self):
        event = SOURCE.split('static bool evdev_handle_mapping_event(', 1)[1].split(
            '\nstatic void evdev_drain(', 1)[0]
        self.assertIn('moonlight_mapping_axis_released(', event)
        self.assertIn('moonlight_mapping_hat_pressed(direction)', event)
        self.assertIn('hat_index == *currentHat && direction == 0', event)
        self.assertIn('if (*currentAbs >= 0)', event)

    def test_trigger_direction_captured_separately(self):
        section = SOURCE.split("static bool evdev_map_abskey(", 1)[1].split(
            "\nbool evdev_map(", 1)[0]
        self.assertIn("currentHalfAxis = halfaxis;", section)
        self.assertIn("*halfaxis = 0;", section)
        self.assertIn("evdev_snapshot_mapping_rest();", SOURCE)
        self.assertIn("dev->mapping_rest[ev->code]", SOURCE)
        self.assertIn("moonlight_mapping_trigger_mode(", SOURCE)

if __name__ == "__main__":
    unittest.main()
