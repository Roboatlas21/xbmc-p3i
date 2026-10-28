import json
import os
import re
import subprocess

LINE = re.compile(r'^card\s+(\d+):.*?device\s+(\d+):\s*(.*?)\s*$')


def _busy(card, device):
    node = '/dev/snd/pcmC%dD%dp' % (card, device)
    if not os.path.exists(node):
        return False
    try:
        out = subprocess.check_output(['fuser', node], stderr=subprocess.DEVNULL,
                                      encoding='utf-8')
    except (subprocess.CalledProcessError, OSError):
        return False
    return bool(out.strip())


def list_playback():
    try:
        raw = subprocess.check_output(['aplay', '-l'], stderr=subprocess.DEVNULL,
                                      encoding='utf-8')
    except (subprocess.CalledProcessError, OSError):
        return []

    devices = []
    for line in raw.splitlines():
        match = LINE.match(line.strip())
        if not match:
            continue

        card, device = int(match.group(1)), int(match.group(2))
        name = match.group(3).replace('[]', '').strip()
        devices.append({
            'alsa': 'hw:%d,%d' % (card, device),
            'name': name,
            'busy': _busy(card, device),
            'dummy': 'dummy' in name.lower(),
        })

    plugins = _plugin_devices()
    return plugins if plugins else devices


def _kodi_labels():
    try:
        import xbmc
    except ImportError:
        return {}

    request = json.dumps({
        'jsonrpc': '2.0',
        'id': 1,
        'method': 'Settings.GetSettings',
        'params': {'level': 'expert',
                   'filter': {'section': 'system', 'category': 'audio'}},
    })
    try:
        answer = json.loads(xbmc.executeJSONRPC(request))
    except (ValueError, TypeError, RuntimeError):
        return {}

    labels = {}
    for setting in answer.get('result', {}).get('settings', []):
        if setting.get('id') != 'audiooutput.audiodevice':
            continue
        for option in setting.get('options', []):
            value = option.get('value') or ''
            label = option.get('label') or ''
            if not value.startswith('ALSA:') or not label:
                continue
            labels[value[len('ALSA:'):].split('|')[0]] = label
    return labels


def _labels_for(names):
    labels = _kodi_labels()

    parts = {}
    for name in names:
        label = labels.get(name)
        if not label:
            continue
        head = label[len('ALSA: '):] if label.startswith('ALSA: ') else label
        card, _, tail = head.partition(', ')
        parts[name] = (card, tail or card)

    cards = set(card for card, _ in parts.values())
    if len(cards) > 1:
        return dict((n, '%s, %s' % parts[n]) for n in parts)
    return dict((n, parts[n][1]) for n in parts)


def pretty(alsa):
    for device in list_playback():
        if device['alsa'] == alsa:
            return device['name']
    return _labels_for([alsa]).get(alsa, alsa)


PLUGIN_PREFIXES = ('surround', 'iec958')

PLUGIN_DEV = re.compile(r'DEV=(\d+)')


def _plugin_devices():
    try:
        raw = subprocess.check_output(['aplay', '-L'], stderr=subprocess.DEVNULL,
                                      encoding='utf-8')
    except (subprocess.CalledProcessError, OSError):
        return []

    devices = []
    name = None
    description = ''

    def flush():
        if not name:
            return
        match = PLUGIN_DEV.search(name)
        device = int(match.group(1)) if match else None
        devices.append({
            'alsa': name,
            'name': description or name,
            'busy': _busy(0, device) if device is not None else False,
            'dummy': False,
        })

    for line in raw.splitlines():
        if line and not line[0].isspace():
            flush()
            name = line.strip() if line.strip().startswith(PLUGIN_PREFIXES) else None
            description = ''
            continue

        if name and line.strip().rstrip(','):
            description = line.strip()

    flush()

    labels = _labels_for([d['alsa'] for d in devices])
    for device in devices:
        if labels.get(device['alsa']):
            device['name'] = labels[device['alsa']]
    return devices


STEREO_ONLY = ('iec958', 'spdif')


def multichannel(alsa):
    return not (alsa or '').startswith(STEREO_ONLY)


def default_device(devices=None):
    if devices is None:
        devices = list_playback()

    for prefix in ('surround', 'iec958'):
        for device in devices:
            if device['alsa'].startswith(prefix):
                return device['alsa']

    for device in devices:
        if not device['dummy']:
            return device['alsa']
    return devices[0]['alsa'] if devices else None


def describe(device, busy_label='Busy'):
    name = device['name'] or device['alsa']
    if device['busy']:
        return '%s  (%s)' % (name, busy_label)
    return name
