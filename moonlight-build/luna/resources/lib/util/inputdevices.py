import io
import re

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
        name = phys = handlers = ''
        for line in block.splitlines():
            if line.startswith('N: Name='):
                name = line.split('=', 1)[1].strip().strip('"')
            elif line.startswith('P: Phys='):
                phys = line.split('=', 1)[1].strip()
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
            'usable': _usable(event) and name not in COVERED_BY_CEC,
        })
    return devices


def resolve(names):
    wanted = set(names)
    return [d['path'] for d in list_devices() if d['name'] in wanted]


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
