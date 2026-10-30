import io
import re
import json


class InvalidInputSelection(ValueError):
    """Saved controller identities are invalid; do not start unfiltered input."""


PROC = '/proc/bus/input/devices'

SYSTEM_NAMES = ('cec_input', 'input_ethrcu', 'input_btrcu', 'gpio_keypad',
                'meson-ir', 'eventlircd')

COVERED_BY_CEC = ('cec_input',)

KEYS_ENOUGH = 8

BTN_LEFT = 272
BTN_TOUCH = 330
BTN_GAMEPAD = 304

EVENT = re.compile(r'\bevent(\d+)\b')
MOUSE = re.compile(r'\bmouse\d+\b')
JOYSTICK = re.compile(r'\bjs\d+\b')


def _blocks():
    try:
        text = io.open(PROC, encoding='utf-8', errors='replace').read()
    except IOError:
        return []
    return [b for b in text.split('\n\n') if b.strip()]


def _kind(handlers):
    if JOYSTICK.search(handlers):
        return 'gamepad'
    if MOUSE.search(handlers):
        return 'mouse'
    if 'kbd' in handlers:
        return 'keyboard'
    return 'other'


def _mask(event, kind):
    path = '/sys/class/input/%s/device/capabilities/%s' % (event, kind)
    try:
        words = io.open(path).read().split()
    except (IOError, OSError):
        return 0
    value = 0
    for i, word in enumerate(reversed(words)):
        value |= int(word, 16) << (64 * i)
    return value


def _usable(event):
    keys = _mask(event, 'key')
    pointer = _mask(event, 'rel') or _mask(event, 'abs')
    has_button = keys >> BTN_LEFT & 1 or keys >> BTN_TOUCH & 1
    gamepad = keys >> BTN_GAMEPAD & 1

    if gamepad:
        return True
    if pointer and has_button:
        return True
    return bin(keys).count('1') >= KEYS_ENOUGH


def list_devices():
    devices = []
    for block in _blocks():
        name = phys = uniq = handlers = ''
        for line in block.splitlines():
            if line.startswith('N: Name='):
                name = line.split('=', 1)[1].strip().strip('"')
            elif line.startswith('P: Phys='):
                phys = line.split('=', 1)[1].strip()
            elif line.startswith('U: Uniq='):
                uniq = line.split('=', 1)[1].strip()
            elif line.startswith('H: Handlers='):
                handlers = line.split('=', 1)[1].strip()

        match = EVENT.search(handlers)
        if not match or not name:
            continue

        event = 'event%s' % match.group(1)
        devices.append({
            'name': name,
            'path': '/dev/input/%s' % event,
            'kind': _kind(handlers),
            'system': name in SYSTEM_NAMES,
            'phys': phys,
            'uniq': uniq,
            'usable': _usable(event) and name not in COVERED_BY_CEC,
        })
    return devices


def resolve(names):
    wanted = set(names)
    return [d['path'] for d in list_devices() if d['name'] in wanted]

# New selections keep name + stable evdev identity. U: Uniq is usually the
# Bluetooth address or USB serial; P: Phys identifies a stable USB port when
# uniq is empty/non-unique. Never save eventN: those numbers are reused.
def _valid_text(value, allow_empty=False):
    if (not isinstance(value, str) or '\x00' in value or
            (not allow_empty and not value)):
        return False
    try:
        return len(value.encode('utf-8')) <= 255
    except UnicodeError:
        return False


def _trusted_unique_id(value):
    """Prefer serials/MAC-like IDs; do not bind generic IDs to every port.

    Common kernel 'Uniq' placeholders (zeroes, repeated single characters,
    or generic strings) are insufficient to distinguish controllers.
    """
    if not _valid_text(value):
        return False
    normalized = value.strip().lower()
    if normalized in ('none', 'unknown', 'default', 'generic', 'null',
                      'same', '0', '0000', '1234'):
        return False
    # Match placeholders as WHOLE identifiers, not their hex-looking
    # fragments. For example BT-B is a valid ID: stripping non-hex
    # characters produces 'bb' and previously widened it to a name group.
    compact = re.sub(r'[-:._]', '', normalized)
    if compact and len(set(compact)) == 1:
        return False
    return len(normalized) >= 4 and len(set(normalized)) >= 2


def select_identity(device, peers=None):
    name = device['name']
    peers = peers if peers is not None else [device]
    uniq = device.get('uniq', '') or ''
    unique_now = not any(
        other is not device and other.get('name') == name and
        other.get('uniq') == uniq for other in peers)
    if unique_now and _trusted_unique_id(uniq):
        # Real serials remain valid across port changes and reconnections.
        return {'name': name, 'kind': 'U', 'value': uniq}
    phys = device.get('phys', '') or ''
    if _valid_text(phys) and not any(
            other is not device and other.get('name') == name and
            other.get('phys') == phys for other in peers):
        # No trustworthy serial; bind generic IDs to an explicit endpoint.
        return {'name': name, 'kind': 'P', 'value': phys}
    # A generic ID without a physical path cannot distinguish a second
    # same-model device that connects later. Keep the explicit name group
    # instead of claiming individual identity we cannot reliably verify.
    # No distinguishable physical identity: selecting this name selects all
    # devices that report it. Do not pretend they can be selected separately.
    return {'name': name, 'kind': 'N', 'value': ''}


def _validate_selector(item):
    if not isinstance(item, dict):
        raise ValueError('Invalid saved input selector')
    name, kind, value = item.get('name'), item.get('kind'), item.get('value')
    physical = item.get('phys', '')
    if (not _valid_text(name) or kind not in ('U', 'P', 'N') or
            not _valid_text(value, allow_empty=True) or
            not _valid_text(physical, allow_empty=True) or
            ((kind == 'N') != (value == '')) or
            (kind != 'U' and physical)):
        raise InvalidInputSelection('Invalid saved controller identity')
    record = {'name': name, 'kind': kind, 'value': value}
    if physical:
        record['phys'] = physical
    return record


def load_selectors(plugin):
    raw = plugin.getSetting('input_device_selectors_v2')
    if raw:
        try:
            data = json.loads(raw)
            if (not isinstance(data, dict) or data.get('schema') != 2 or
                    not isinstance(data.get('devices'), list) or
                    len(data['devices']) > 64):
                raise ValueError('Bad selected-input format')
            return [_validate_selector(item) for item in data['devices']]
        except (ValueError, TypeError, UnicodeError) as error:
            raise InvalidInputSelection(
                'Saved controller selection is invalid. Open Luna Settings, '
                'choose Input Devices, and reselect or clear your controllers.'
            ) from error
    # Legacy preferences only stored names. Preserve them until the user
    # visits the picker again and selects per-device identities.
    return [_validate_selector({'name': n, 'kind': 'N', 'value': ''})
            for n in plugin.getSetting('input_devices').split('|') if n]


def serialize_selectors(selectors):
    records = [_validate_selector(item) for item in selectors]
    return json.dumps({'schema': 2, 'devices': records},
                      ensure_ascii=False, separators=(',', ':'))


def match_selector(device, selector):
    if device.get('name') != selector['name']:
        return False
    kind = selector['kind']
    if kind == 'N':
        return True
    field = 'uniq' if kind == 'U' else 'phys'
    return (bool(selector['value']) and
            device.get(field) == selector['value'] and
            (kind != 'U' or not selector.get('phys') or
             device.get('phys') == selector['phys']))


def picker_options(all_devices, saved):
    """Include selected disconnected devices, allowing deliberate deselection.

    Each option contains a currently connected device or a saved offline
    identity. Never drop an offline selection just because it is absent from
    /proc/bus/input/devices while this dialog is open.
    """
    candidates = [device for device in all_devices if device.get('usable') or
                  any(match_selector(device, entry) for entry in saved)]
    options = []
    # Legacy name-only preferences cover all same-name devices, including
    # disconnected ones. Keep that group as its own selected option until
    # explicitly deselected; never auto-migrate it to one online device.
    for entry in saved:
        if entry['kind'] == 'N' and entry not in [
                option['selector'] for option in options]:
            options.append({
                'device': None, 'selector': entry, 'selected': True,
                'offline': not any(d['name'] == entry['name'] for d in candidates),
                'group': True,
            })
    for device in candidates:
        selector = select_identity(device, candidates)
        if any(option['selector'] == selector for option in options):
            continue
        # A legacy composite U+Phys entry can be migrated back to a strong
        # serial without requiring the original port. A weak serial never
        # authorizes a different port without explicit reselection.
        upgraded = any(
            entry['kind'] == 'U' and selector['kind'] == 'U' and
            entry['name'] == selector['name'] and
            entry['value'] == selector['value'] and
            _trusted_unique_id(selector['value'])
            for entry in saved)
        options.append({
            'device': device,
            'selector': selector,
            'selected': upgraded or any(
                match_selector(device, entry) for entry in saved
                if entry['kind'] != 'N'),
            'offline': False,
            'group': False,
        })
    for entry in saved:
        if entry['kind'] == 'N':
            continue
        if (not any(option['selected'] and
                    option['selector']['name'] == entry['name'] and
                    option['selector']['kind'] == 'U' and
                    entry['kind'] == 'U' and
                    option['selector']['value'] == entry['value'] and
                    _trusted_unique_id(entry['value'])
                    for option in options) and
            not any(match_selector(device, entry) for device in candidates) and
            not any(option['selector'] == entry for option in options)):
            options.append({
                'device': None, 'selector': entry, 'selected': True,
                'offline': True, 'group': False,
            })
    return options


def resolve_selectors(selectors, devices=None):
    devices = list_devices() if devices is None else devices
    return [d['path'] for d in devices
            if any(match_selector(d, selector) for selector in selectors)]


def encode_selectors(selectors):
    tokens = []
    for raw in selectors:
        selector = _validate_selector(raw)
        name = selector['name'].encode('utf-8').hex()
        value = selector['value'].encode('utf-8').hex()
        token = '%s:%s:%s' % (name, selector['kind'], value)
        if selector.get('phys'):
            token += ':' + selector['phys'].encode('utf-8').hex()
        tokens.append(token)
    result = ','.join(dict.fromkeys(tokens))
    if len(result) > 16384:
        raise ValueError('Selected controller identities exceed native limit')
    return result


KIND_LABELS = {
    'keyboard': 'keyboard',
    'mouse': 'mouse',
    'gamepad': 'gamepad',
    'other': 'other',
}
SYSTEM_LABEL = 'system'


def describe(device, kinds=None, system_label=None):
    kinds = kinds or KIND_LABELS
    mark = ' [%s]' % (system_label or SYSTEM_LABEL) if device['system'] else ''
    return '%s  (%s)%s' % (device['name'],
                           kinds.get(device['kind'], device['kind']), mark)
