import configparser
import io
import os
import struct
import tempfile
import fcntl
import xbmcaddon

from resources.lib.util import audiodevices
from resources.lib.util import inputdevices

class ConfigHelper:
    config_path = None
    addon_path = None
    binary_path = None
    launchscripts_path = None
    logger = None
    plugin = None
    config = {}

    def __init__(self, plugin, logger):
        self.plugin = plugin
        # Kodi's getAddonInfo('path') may or may not include a trailing
        # separator. Multiple Luna callers append paths directly.
        self.addon_path = os.path.join(xbmcaddon.Addon().getAddonInfo('path'), '')
        self.logger = logger
        self._reset()
        self.configure(False)


    def _reset(self):
        for elem in self.config:
            self.config[elem] = None

    def configure(self, dump=True):
        self.binary_path = self._find_binary(self.addon_path, logger=self.logger)

        # get_path("moonlight.conf") checks Moonlight's CWD first. Kodi uses
        # the bundled architecture-specific binary directory as that CWD.
        # /etc/moonlight.conf is not one of get_path()'s default paths.
        bundled_bin = os.path.join(self.addon_path, 'bin')
        managed = (self.binary_path is not None and
                   os.path.commonpath((bundled_bin,
                                       os.path.abspath(self.binary_path))) == bundled_bin)
        if managed:
            self.config_path = os.path.join(self.binary_path, 'moonlight.conf')
        elif os.access('/etc', os.W_OK):
            self.config_path = '/etc/moonlight.conf'
        elif self.binary_path is not None and os.access(self.binary_path, os.W_OK):
            self.config_path = self.binary_path + 'moonlight.conf'
        else:
            self.logger.info('Moonlight binary could not be found. Configuration file saved at %s' % self.addon_path)
            self.config_path = self.addon_path + 'moonlight.conf'
        
        launchscript_name = self.plugin.getSetting('launchscript_conf')
        if launchscript_name:
            self.launchscripts_path = self.addon_path + 'resources/launchscripts/' + launchscript_name + '/'

        self.config = {
            'host_addr':                    self.plugin.getSetting('host_addr'),
            'resolution':                   self.plugin.getSetting('resolution'),
            'resolution_width':             self.plugin.getSetting('resolution_width'),
            'resolution_height':            self.plugin.getSetting('resolution_height'),
            'codec':                        self.plugin.getSetting('codec'),
            'bitrate':                      self.plugin.getSettingInt('bitrate'),
            'framerate':                    self.plugin.getSetting('framerate'),
            'surround':                     self.plugin.getSetting('surround'),
            'audio_device_parameter':       self.plugin.getSetting('audio_device_parameter'),
            'packetsize':                   self.plugin.getSettingInt('packetsize'),
            'gamepad_mouse':                self.plugin.getSetting('gamepad_mouse'),
            'cec':                          self.plugin.getSetting('cec'),
            'hdr_flag':                     self.plugin.getSetting('hdr_flag'),
            'remote_optimizations':         self.plugin.getSetting('remote_optimizations'),
            'local_audio':                  self.plugin.getSetting('local_audio'),
            'debug_mode':                   self.plugin.getSetting('debug_mode'),
            'osd':                          self.plugin.getSetting('osd'),
            'osd_font_size':                self.plugin.getSetting('osd_font_size'),
            'osd_position':                 self.plugin.getSetting('osd_position'),
            'osd_top':                      self.plugin.getSetting('osd_top'),
            'osd_interval':                 self.plugin.getSetting('osd_interval'),
        }

        if not self.config['host_addr']:
            self.config['host_addr'] = self._address_from_conf()

        if dump:
            self._dump_conf()

    def _address_from_conf(self):
        try:
            with io.open(self.config_path, encoding='utf-8') as handle:
                for line in handle:
                    if line.startswith('address'):
                        parts = line.split('=', 1)
                        if len(parts) == 2:
                            return parts[1].strip()
        except (IOError, OSError):
            pass
        return ''

    def _osd_args(self):
        args = [
            '-font-size', self.config['osd_font_size'] or '20',
            '-y', self.config['osd_top'] or '40',
            '-interval', self.config['osd_interval'] or '1.5',
        ]

        if self.config['osd_position'] == 'left':
            args += ['-x', '20']

        if self.config['debug_mode'] == 'true':
            args += ['-log']

        return ' '.join(args)

    def dump(self):
        self._dump_conf()

    def _dump_conf(self):
        # Guard read/modify/write as one transaction across Kodi invocations.
        # The file itself is atomically replaced, so readers never see a
        # truncated configuration even if they do not take the lock.
        with io.open(self.config_path + '.lock', 'a+') as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            try:
                return self._dump_conf_locked()
            finally:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

    def _dump_conf_locked(self):
        config = configparser.ConfigParser(strict=False)
        config.read(self.config_path, encoding='utf-8')

        if 'Moonlight' not in config.sections():
            config.add_section('Moonlight')

        config.remove_option('Moonlight', 'input')

        if self.config['host_addr']:
            config.set('Moonlight', 'address', self.config['host_addr'])
        else:
            config.remove_option('Moonlight', 'address')
        
        resolution_width = '1280'
        resolution_height = '720'
        if self.config['resolution'] == 'Custom':
            resolution_width = self.config['resolution_width']
            resolution_height = self.config['resolution_height']
        else:
            res = self.config['resolution'].split('x')
            resolution_width = res[0]
            resolution_height = res[1]

        config.set('Moonlight', 'width', resolution_width)
        config.set('Moonlight', 'height', resolution_height)
        
        
        config.set('Moonlight', 'fps', self.config['framerate'])

        config.set('Moonlight', 'bitrate', str(self.config['bitrate'] * 1000))
        config.set('Moonlight', 'packetsize', str(self.config['packetsize']))
        config.set('Moonlight', 'sops', 'false')
        config.set('Moonlight', 'remote', self.config['remote_optimizations'])
        config.set('Moonlight', 'localaudio', self.config['local_audio'])
        config.set('Moonlight', 'debug', self.config['debug_mode'])
        
        multichannel = True

        if self.config['local_audio'] == 'true':
            config.remove_option('Moonlight', 'audio')
        else:
            device = self.config['audio_device_parameter']

            devices = audiodevices.list_playback()
            known = dict((d['alsa'], d['name']) for d in devices)

            if device and known and device not in known:
                device = ''

            if not device:
                device = audiodevices.default_device(devices)
                if device:
                    self.plugin.setSettingString('audio_device_parameter', device)
            if device:
                self.plugin.setSettingString('audio_device_display',
                                             known.get(device, device))
                multichannel = audiodevices.multichannel(device)
                self.plugin.setSettingString(
                    'audio_multichannel', 'true' if multichannel else 'false')
                config.set('Moonlight', 'audio', device)
            else:
                config.remove_option('Moonlight', 'audio')

        config.set('Moonlight', 'codec', self.config['codec'])
        if self.config['surround'] != 'false' and multichannel:
            config.set('Moonlight', 'surround', self.config['surround'])
        else:
            config.remove_option('Moonlight', 'surround')
        
        config.set('Moonlight', 'nounsupported', 'false')
        gamepad_mouse = self.config['gamepad_mouse'] != 'false'
        config.set('Moonlight', 'nomouseemulation',
                   'false' if gamepad_mouse else 'true')
        config.set('Moonlight', 'hdr', self.config['hdr_flag'])

        config.set('Moonlight', 'cec',
                   'false' if self.config['cec'] == 'false' else 'true')

        config.set('Moonlight', 'osd', self.config['osd'] or 'false')
        config.set('Moonlight', 'osdargs', self._osd_args())

        selectors = inputdevices.load_selectors(self.plugin)
        paths = inputdevices.resolve_selectors(selectors)

        rendered = io.StringIO()
        config.write(rendered)
        for path in paths:
            rendered.write('input = %s\n' % path)
        content = rendered.getvalue()
        self._atomic_config_write(self.config_path, content)
        # An individual launch must capture the exact settings rendered by
        # THIS Kodi invocation, not a later update to shared moonlight.conf.
        self._launch_config_content = content
        self.logger.info('[ConfigHelper] - Dumped config to disk.')

    @staticmethod
    def _atomic_config_write(path, content):
        directory = os.path.dirname(path)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(
                    mode='w', encoding='utf-8', dir=directory,
                    prefix='.luna-config-', suffix='.tmp',
                    delete=False) as stream:
                temporary = stream.name
                os.chmod(temporary, 0o600)
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            if temporary and os.path.exists(temporary):
                os.unlink(temporary)

    def create_launch_snapshot(self):
        """Private immutable-per-invocation config for preflight and stream."""
        content = getattr(self, '_launch_config_content', None)
        if content is None:
            raise ValueError('Luna launch configuration has not been generated')
        folder = os.path.dirname(self.config_path)
        fd, snapshot = tempfile.mkstemp(
            prefix='.luna-stream-', suffix='.conf', dir=folder)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as stream:
                os.fchmod(stream.fileno(), 0o600)
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            return snapshot
        except BaseException:
            try:
                os.unlink(snapshot)
            except OSError:
                pass
            raise

    @staticmethod
    def _find_binary(internal_path, logger=None):
        # Diagnose the first attempt without hiding startup failures behind retries.
        elf_class = 1 if struct.calcsize('P') == 4 else 2
        arch = 'coreelec-ng' if elf_class == 1 else 'coreelec-no'
        candidates = [
            os.path.join(internal_path, 'bin', arch, 'moonlight'),
            '/usr/bin/moonlight',
            '/usr/local/bin/moonlight',
            '/storage/moonlight/moonlight',
            os.path.join(internal_path, 'bin', 'moonlight'),
        ]
        for candidate in candidates:
            try:
                if not os.path.isfile(candidate):
                    reason = 'missing'
                elif not os.access(candidate, os.X_OK):
                    reason = 'not executable'
                else:
                    with open(candidate, 'rb') as executable:
                        header = executable.read(5)
                    if len(header) != 5 or header[:4] != bytes((127,)) + b'ELF':
                        reason = 'invalid ELF header'
                    elif header[4] != elf_class:
                        reason = 'wrong ELF class (expected %d, got %d)' % (elf_class, header[4])
                    else:
                        selected = os.path.dirname(candidate) + '/'
                        if logger:
                            bundled = os.path.join(internal_path, 'bin')
                            origin = ('bundled' if os.path.commonpath(
                                (os.path.abspath(candidate), os.path.abspath(bundled))) ==
                                os.path.abspath(bundled) else 'system/external')
                            logger.info('[ConfigHelper] - Selected Moonlight executable: %s (%s, ELF class %d)' %
                                        (candidate, origin, elf_class))
                        return selected
            except OSError as error:
                reason = 'read error: %s' % error
            if logger:
                logger.info('[ConfigHelper] - Moonlight candidate %s: %s' % (candidate, reason))
        if logger:
            logger.info('[ConfigHelper] - No usable Moonlight executable (addon=%s, expected ELF class=%d, candidates=%d)' %
                        (internal_path, elf_class, len(candidates)))
        return None
