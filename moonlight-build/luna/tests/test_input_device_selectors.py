"""Exercise stable per-device controller selection without a Kodi runtime."""
import unittest
from resources.lib.util import inputdevices


class MockSettings:
    def __init__(self, **values):
        self.values = values

    def getSetting(self, key):
        return self.values.get(key, '')


def device(name, path, uniq='', phys=''):
    return dict(name=name, path=path, uniq=uniq, phys=phys)


class InputDeviceSelectorsTests(unittest.TestCase):
    def test_two_identical_bluetooth_controllers_are_independent(self):
        first = device('Xbox Pad', '/dev/input/event2', uniq='BT-A')
        second = device('Xbox Pad', '/dev/input/event9', uniq='BT-B')
        selected = inputdevices.select_identity(first, [first, second])
        self.assertEqual(selected, {'name': 'Xbox Pad', 'kind': 'U', 'value': 'BT-A'})
        self.assertEqual(inputdevices.resolve_selectors([selected], [first, second]),
                         ['/dev/input/event2'])
        reconnected = device('Xbox Pad', '/dev/input/event15', uniq='BT-A')
        self.assertEqual(inputdevices.resolve_selectors(
            [selected], [second, reconnected]), ['/dev/input/event15'])
        self.assertEqual(inputdevices.encode_selectors([selected]),
                         '58626f7820506164:U:42542d41')
        selected_second = inputdevices.select_identity(second, [first, second])
        self.assertEqual(selected_second, {
            'name': 'Xbox Pad', 'kind': 'U', 'value': 'BT-B'})
        self.assertEqual(inputdevices.resolve_selectors(
            [selected_second], [first, second]), ['/dev/input/event9'])
        self.assertEqual(inputdevices.encode_selectors([selected_second]),
                         '58626f7820506164:U:42542d42')

    def test_identifier_classification_does_not_strip_nonhex_characters(self):
        for identifier in ('BT-B', 'MAC-A', 'usb-B-3', 'BT-F'):
            with self.subTest(identifier=identifier):
                self.assertTrue(inputdevices._trusted_unique_id(identifier))
        for identifier in ('0000', 'FFFF', '1111', 'SAME', 'unknown'):
            with self.subTest(identifier=identifier):
                self.assertFalse(inputdevices._trusted_unique_id(identifier))

    def test_duplicate_uniq_falls_back_to_distinct_physical_path(self):
        first = device('Pad', '/dev/input/event1', uniq='SAME', phys='usb-1/input0')
        second = device('Pad', '/dev/input/event2', uniq='SAME', phys='usb-2/input0')
        selected = inputdevices.select_identity(first, [first, second])
        self.assertEqual(selected['kind'], 'P')
        self.assertEqual(inputdevices.resolve_selectors([selected], [first, second]),
                         ['/dev/input/event1'])

    def test_indistinguishable_controllers_group_into_shared_name(self):
        first = device('Generic Pad', '/dev/input/event1')
        second = device('Generic Pad', '/dev/input/event2')
        selected = inputdevices.select_identity(first, [first, second])
        self.assertEqual(selected['kind'], 'N')
        self.assertEqual(inputdevices.resolve_selectors([selected], [first, second]),
                         ['/dev/input/event1', '/dev/input/event2'])

    def test_generic_serial_uses_physical_port_and_blocks_late_duplicate(self):
        first = device('Xbox Pad', '/dev/input/event2',
                       uniq='0000', phys='usb-1/input0')
        selected = inputdevices.select_identity(first, [first])
        self.assertEqual(selected, {'name': 'Xbox Pad', 'kind': 'P',
                                    'value': 'usb-1/input0'})
        second = device('Xbox Pad', '/dev/input/event8',
                        uniq='0000', phys='usb-2/input0')
        self.assertEqual(inputdevices.resolve_selectors(
            [selected], [first, second]), ['/dev/input/event2'])
        reconnected = device('Xbox Pad', '/dev/input/event15',
                             uniq='0000', phys='usb-1/input0')
        self.assertEqual(inputdevices.resolve_selectors(
            [selected], [second, reconnected]), ['/dev/input/event15'])
        self.assertEqual(inputdevices.encode_selectors([selected]),
                         '58626f7820506164:P:7573622d312f696e70757430')

    def test_generic_uniq_without_phys_cannot_be_individually_authorized(self):
        only = device('Generic Pad', '/dev/input/event3', uniq='0000')
        selected = inputdevices.select_identity(only, [only])
        self.assertEqual(selected, {'name': 'Generic Pad', 'kind': 'N',
                                    'value': ''})

    def test_trustworthy_serial_survives_changed_usb_port(self):
        first = device('Xbox Pad', '/dev/input/event2',
                       uniq='AB12-XY98', phys='usb-1/input0')
        selected = inputdevices.select_identity(first, [first])
        self.assertEqual(selected, {'name': 'Xbox Pad', 'kind': 'U',
                                    'value': 'AB12-XY98'})
        new_port = device('Xbox Pad', '/dev/input/event9',
                          uniq='AB12-XY98', phys='usb-2/input0')
        self.assertEqual(inputdevices.resolve_selectors([selected], [new_port]),
                         ['/dev/input/event9'])
        # Last revision's composite selection is still understood and
        # explicitly upgraded to the trustworthy serial when visible.
        old = dict(selected, phys='usb-1/input0')
        new_port['usable'] = True
        options = inputdevices.picker_options([new_port], [old])
        self.assertEqual(len(options), 1)
        self.assertTrue(options[0]['selected'])
        self.assertEqual(options[0]['selector'], selected)

    def test_offline_selected_controllers_are_preserved_until_deselected(self):
        first = {'name': 'Xbox Pad', 'kind': 'U',
                 'value': 'BT-A', 'phys': 'usb-1/input0'}
        second = {'name': 'Xbox Pad', 'kind': 'U',
                  'value': 'BT-B', 'phys': 'usb-2/input0'}
        live = [device('Xbox Pad', '/dev/input/event3',
                       uniq='BT-A', phys='usb-1/input0')]
        live[0]['usable'] = True
        options = inputdevices.picker_options(live, [first, second])
        self.assertEqual(len(options), 2)
        self.assertFalse(options[0]['offline'])
        self.assertTrue(options[1]['offline'])
        self.assertTrue(all(row['selected'] for row in options))
        self.assertEqual(options[0]['selector'],
                         {'name': 'Xbox Pad', 'kind': 'U', 'value': 'BT-A'})
        self.assertEqual(options[1]['selector'], second)
        # Saving another selection does not implicitly drop an offline device.
        records = [row['selector'] for row in options if row['selected']]
        self.assertEqual(inputdevices.load_selectors(
            MockSettings(input_device_selectors_v2=
                         inputdevices.serialize_selectors(records))),
                         records)
        # Deselecting the offline entry removes it intentionally. The live
        # identity is now serial-only, so do not compare to an old U+Phys row.
        self.assertEqual([options[0]['selector']], [
            {'name': 'Xbox Pad', 'kind': 'U', 'value': 'BT-A'}])
        self.assertTrue(options[1]['selected'] and options[1]['offline'])
        self.assertEqual(options[1]['selector'], second)

    def test_picker_supports_all_saved_devices_offline(self):
        saved = [{'name': 'Pad', 'kind': 'P', 'value': 'usb-1/input0'}]
        options = inputdevices.picker_options([], saved)
        self.assertEqual(len(options), 1)
        self.assertTrue(options[0]['selected'] and options[0]['offline'])
        self.assertEqual(options[0]['selector'], saved[0])

    def test_legacy_name_group_remains_selected_when_peer_is_offline(self):
        saved = [{'name': 'Xbox Pad', 'kind': 'N', 'value': ''}]
        connected = [device('Xbox Pad', '/dev/input/event1',
                            uniq='MAC-A', phys='usb-1/input0')]
        connected[0]['usable'] = True
        options = inputdevices.picker_options(connected, saved)
        self.assertEqual(len(options), 2)
        self.assertTrue(options[0]['group'] and options[0]['selected'])
        self.assertEqual(options[0]['selector'], saved[0])
        self.assertFalse(options[1]['selected'])
        self.assertEqual(options[1]['selector']['kind'], 'U')
        # A second, currently offline controller of the same name stays
        # covered by the original group if the user makes no change.
        self.assertEqual([o['selector'] for o in options if o['selected']],
                         saved)
        # Moving from the group to individual selection requires explicitly
        # unchecking the group and checking the individual option.
        self.assertEqual(options[1]['selector']['value'], 'MAC-A')

    def test_saved_records_roundtrip_and_legacy_compatibility(self):
        selected = [{'name': 'Pad', 'kind': 'U', 'value': 'BT-A'}]
        raw = inputdevices.serialize_selectors(selected)
        self.assertEqual(inputdevices.load_selectors(
            MockSettings(input_device_selectors_v2=raw)), selected)
        legacy = inputdevices.load_selectors(MockSettings(input_devices='Pad'))
        self.assertEqual(legacy, [{'name': 'Pad', 'kind': 'N', 'value': ''}])
        self.assertEqual(inputdevices.encode_selectors(legacy), '506164:N:')

    def test_invalid_v2_never_silently_falls_back_to_names(self):
        with self.assertRaises(inputdevices.InvalidInputSelection) as invalid:
            inputdevices.load_selectors(MockSettings(
                input_device_selectors_v2='corrupt', input_devices='Keyboard'))
        self.assertIn('Open Luna Settings', str(invalid.exception))
        with self.assertRaises(ValueError):
            inputdevices.encode_selectors([{'name': 'Keyboard',
                                            'kind': 'U', 'value': ''}])


if __name__ == '__main__':
    unittest.main()
