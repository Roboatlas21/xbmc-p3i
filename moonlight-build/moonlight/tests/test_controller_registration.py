"""Count only actual evdev registrations, including hotplug removal."""
from pathlib import Path
import unittest

CODE = (Path(__file__).resolve().parents[1] / 'src/input/evdev.c').read_text()


class ControllerRegistrationTests(unittest.TestCase):
    def test_registration_count_after_append_and_poll_subscription(self):
        create = CODE.split('void evdev_create(', 1)[1].split('static void evdev_map_key(', 1)[0]
        self.assertEqual(create.count('++evdev_gamepads'), 1)
        self.assertLess(create.index('input_slots_append('), create.index('++evdev_gamepads'))
        self.assertLess(create.index('loop_add_fd('), create.index('++evdev_gamepads'))
        self.assertIn('devices[dev]->is_gamepad = is_gamepad && mappings != NULL;', create)
        self.assertIn('devices[dev]->map = NULL;', create)
        self.assertIn('devices[dev]->is_gamepad = false;', create)
        self.assertIn('if (devices[dev]->is_gamepad)\n    ++evdev_gamepads;', create)
        self.assertIn('evdev_fd_already_registered(fd)', create)

    def test_removal_decrements_only_counted_gamepads(self):
        remove = CODE.split('static void evdev_remove(', 1)[1].split('static short evdev_convert_value(', 1)[0]
        self.assertIn('device->is_gamepad && evdev_gamepads > 0', remove)
        self.assertIn('--evdev_gamepads;', remove)


if __name__ == '__main__':
    unittest.main()
