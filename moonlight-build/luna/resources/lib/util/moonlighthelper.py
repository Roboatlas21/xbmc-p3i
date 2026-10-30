import os
import tempfile
import signal
import subprocess
import queue
import time
import re
import threading
import traceback
import uuid

LUNA_NG_ACK_WAIT_SECONDS = 6.0

def encode_input_names(names):
    """Exact UTF-8 whitelist for evdev hotplug; delimiters cannot escape hex."""
    encoded = []
    for name in names:
        data = name.encode('utf-8')
        if not data or len(data) > 255 or b'\x00' in data:
            raise ValueError('Invalid selected controller device name')
        encoded.append(data.hex())
    result = ','.join(dict.fromkeys(encoded))
    if len(result) > 16384:
        raise ValueError('Selected controller names exceed native whitelist size')
    return result

LUNA_NG_RECOVERY_STEP_SECONDS = 70.0
LUNA_NG_RECOVERY_MAX_SECONDS = 900.0

import xbmc
import xbmcgui

def env_with_libs(binary_path, addon_path=None):
    env = dict(os.environ)
    root = os.path.abspath(addon_path or '')
    selected = os.path.abspath(binary_path or '')
    bin_root = os.path.join(root, 'bin')
    # System-installed Moonlight must not inherit the add-on's shared libraries.
    if not root or not (selected == bin_root or selected.startswith(bin_root + os.sep)):
        return env
    arch = 'coreelec-ng' if __import__('struct').calcsize('P') == 4 else 'coreelec-no'
    private = os.path.join(root, 'lib', arch)
    legacy = os.path.join(root, 'lib')
    if os.path.isdir(private):
        lib = private
    elif selected == bin_root and os.path.isdir(legacy):
        lib = legacy
    else:
        return env
    previous = env.get('LD_LIBRARY_PATH', '')
    env['LD_LIBRARY_PATH'] = lib + (':' + previous if previous else '')
    return env


def moonlight_environment(config_helper):
    return env_with_libs(config_helper.binary_path, config_helper.addon_path)


class MoonlightHelper:

    config_helper = None

    def __init__(self, plugin, config_helper, logger):
        self.plugin = plugin
        self.config_helper = config_helper
        self.logger = logger
        self.pin_received = False
        self._pairing_pin = None
        self.paired_address = None
        self.pairing_messages = ""
        self._pairing_error = None
        self.pair_success = False
        self.last_host_error = None
        self.last_unpair_error = None

    def _pair_line(self, line, dialog):
        text = line.strip()
        if not text:
            return

        # The four-digit pairing PIN belongs in the visible PIN dialog only,
        # never in persistent Kodi logs (including ordinary INFO-level logs).
        if 'enter the following PIN' in line:
            match = re.search(r'\b(\d{4})\b', text)
            self._pairing_pin = match.group(1) if match else None
            xbmc.log('LUNA pairing: PIN prompt displayed [redacted]', xbmc.LOGINFO)
            self.pin_received = True
            dialog.update(50, text)
            return

        safe_text = text
        if getattr(self, '_pairing_pin', None):
            safe_text = safe_text.replace(self._pairing_pin, '[PIN REDACTED]')
        xbmc.log('LUNA pairing: ' + safe_text, xbmc.LOGINFO)

        if text.startswith('Pairing: '):
            # Native progress begins only after the host answers the PIN
            # request, so it cannot hide the PIN while the user needs it.
            dialog.update(75, text[len('Pairing: '):])
            return

        # Keep the authoritative failure even before the PIN is available,
        # and prioritize it over debug output from rollback requests.
        if text.startswith(('Failed to pair to server:', "Can't initialize Moonlight",
                            'Gamestream error:', 'Invalid data received from server:',
                            'Unsupported version:', 'Not enough memory')):
            self._pairing_error = text

        found = re.search(r'Connecting to (\S+?)\.\.\.', line)
        if found:
            self.paired_address = found.group(1)

        if not self.pin_received:
            return
        if line.lstrip().startswith(('Request ', 'Response:', '<?xml', '<root')):
            return
        self.pairing_messages += text + '\n'

    @staticmethod
    def _read_pairing_output(stream, output_queue):
        """Read native Moonlight output without select/TextIO buffering races."""
        try:
            for line in stream:
                output_queue.put(('line', line))
        except Exception as error:
            output_queue.put(('error', error))
        finally:
            output_queue.put(('eof', None))

    def _stop_pairing_process(self, process):
        if process is None or process.poll() is not None:
            return

        try:
            os.killpg(os.getpgid(process.pid), signal.SIGTERM)
        except OSError:
            try:
                process.terminate()
            except OSError:
                pass

        try:
            process.wait(timeout=2)
            return
        except subprocess.TimeoutExpired:
            pass

        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except OSError:
            try:
                process.kill()
            except OSError:
                pass

        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass

    def pair(self):
        # ConfigHelper writes the selected host to moonlight.conf. Do not pass
        # the address positionally too: Moonlight loads the config before CLI
        # parsing and would treat that as a second address.
        self.config_helper.configure()
        binary_path = self.config_helper.binary_path
        address = self.config_helper.config.get('host_addr')

        # Pairing state is per attempt. A cancelled/timed-out attempt must not
        # leak a stale PIN or success message into the next one.
        self.pair_success = False
        self.pin_received = False
        self._pairing_pin = None
        self.paired_address = None
        self.pairing_messages = ""
        self._pairing_error = None

        title = self.plugin.getLocalizedString(30000)
        if not address:
            xbmcgui.Dialog().ok(
                title, 'Set the Sunshine host address before pairing.')
            return False

        try:
            from resources.lib.util.hostapi import Host
            trusted_host = Host(address)
            if trusted_host.has_trusted_pin() and trusted_host.paired():
                self.pair_success = True
                self.paired_address = address
                xbmcgui.Dialog().ok(
                    title,
                    self.plugin.getLocalizedString(30265)
                    or 'Already paired with this host')
                return True
        except Exception:
            # Native pairing below provides the authoritative result.
            pass

        pairing_proc = None
        pairing_thread = None
        pairing_output = queue.Queue()
        dialog = xbmcgui.DialogProgress()
        cancelled = False
        timeout_message = None

        try:
            pairing_proc = subprocess.Popen(
                [binary_path + "moonlight", "pair"],
                cwd=binary_path,
                env=env_with_libs(binary_path, self.config_helper.addon_path),
                encoding='utf-8',
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                bufsize=1,
                start_new_session=True)

            pairing_thread = threading.Thread(
                target=self._read_pairing_output,
                args=(pairing_proc.stdout, pairing_output),
                name='LunaPairingReader',
                daemon=True)
            pairing_thread.start()

            dialog.create(
                title,
                (self.plugin.getLocalizedString(30285)
                 or 'Connecting to Sunshine...'))

            started = time.monotonic()
            pin_deadline = None
            eof_received = False

            while not eof_received:
                if dialog.iscanceled():
                    cancelled = True
                    break

                try:
                    event, value = pairing_output.get(timeout=0.1)
                except queue.Empty:
                    event = None
                    value = None

                if event == 'line':
                    had_pin = self.pin_received
                    self._pair_line(value, dialog)
                    if self.pin_received and not had_pin:
                        pin_deadline = time.monotonic() + 60
                elif event == 'error':
                    raise value
                elif event == 'eof':
                    eof_received = True

                now = time.monotonic()
                if not self.pin_received and now - started >= 20:
                    timeout_message = (
                        'Timed out waiting for a pairing PIN from %s. '
                        'Check that Sunshine is running and reachable.'
                        % address)
                    break
                if (self.pin_received and pin_deadline is not None
                        and now >= pin_deadline):
                    timeout_message = (
                        'Pairing timed out waiting for Sunshine to accept '
                        'the PIN.')
                    break

            if cancelled or timeout_message:
                self._stop_pairing_process(pairing_proc)

            if (not cancelled and not timeout_message
                    and pairing_proc.poll() is None):
                try:
                    pairing_proc.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    timeout_message = 'Native Moonlight did not exit after pairing output.'
                    self._stop_pairing_process(pairing_proc)

            if cancelled:
                return False

            if timeout_message:
                xbmcgui.Dialog().ok(title, timeout_message)
                return False

            reported_success = ('Succesfully paired' in self.pairing_messages
                                or 'Successfully paired' in self.pairing_messages)
            if reported_success and pairing_proc.returncode == 0:
                # A printed success phrase alone is insufficient. The native
                # process must exit successfully, and HTTPS serverinfo must
                # confirm this exact device is now paired.
                try:
                    from resources.lib.util.hostapi import Host
                    trusted_host = Host(address)
                    confirmed = (trusted_host.has_trusted_pin()
                                 and trusted_host.pair_status())
                except Exception as error:
                    xbmcgui.Dialog().ok(
                        title, 'Moonlight reported success, but pairing could '
                        'not be verified: %s' % error)
                    return False
                if not confirmed:
                    self.pairing_messages += (
                        'Sunshine did not establish a trusted certificate. '
                        'Remove any stale pairing in Sunshine and retry.\n')
                if confirmed:
                    self.pair_success = True
                    self.paired_address = address
                    xbmcgui.Dialog().ok(
                        title, self.plugin.getLocalizedString(30286))
                    return True

            if reported_success and pairing_proc.returncode != 0:
                self.pairing_messages += (
                    'Moonlight exited with status %s; pairing was not confirmed.\n'
                    % pairing_proc.returncode)
            elif reported_success:
                self.pairing_messages += (
                    'Sunshine did not confirm pairing over HTTPS.\n')

            message = self._pairing_error or self.pairing_messages.strip()
            if not message:
                message = (self.plugin.getLocalizedString(30287)
                           or 'Pairing failed.')
            xbmcgui.Dialog().ok(title, message)
            return False
        except Exception as e:
            self._stop_pairing_process(pairing_proc)
            xbmcgui.Dialog().ok(
                title, xbmc.getLocalizedString(257) + ':\n' + str(e))
            return False
        finally:
            if pairing_proc is not None and pairing_proc.poll() is None:
                self._stop_pairing_process(pairing_proc)

            if pairing_thread is not None:
                pairing_thread.join(timeout=2)

            try:
                if pairing_proc is not None and pairing_proc.stdout is not None:
                    pairing_proc.stdout.close()
            except OSError:
                pass
            dialog.close()

    def forget_trusted_server(self, address):
        """Deliberate local-only trust reset; does not revoke pairing keys."""
        self.last_trust_error = None
        self.config_helper.configure(dump=False)
        binary_path = self.config_helper.binary_path
        if (not binary_path or not isinstance(address, str) or
                not address.strip() or address.startswith('-') or
                any(char.isspace() for char in address)):
            self.last_trust_error = 'Select a valid host and Moonlight binary.'
            return False
        try:
            result = subprocess.run(
                [binary_path + 'moonlight', 'forget-server-cert', address],
                cwd=binary_path,
                env=env_with_libs(binary_path, self.config_helper.addon_path),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=10, encoding='utf-8', start_new_session=True)
        except (OSError, subprocess.TimeoutExpired) as error:
            self.last_trust_error = 'Unable to clear trusted certificate: %s' % error
            return False
        if result.returncode != 0:
            self.last_trust_error = (result.stdout or '').strip() or (
                'Native trust reset failed (exit %s)' % result.returncode)
            return False
        return True

    def unpair(self):
        self.config_helper.configure()
        binary_path = self.config_helper.binary_path
        address = self.config_helper.config.get('host_addr')
        self.last_unpair_error = None
        self.last_unpair_unverified = False
        if not address or not binary_path:
            self.last_unpair_error = 'No Sunshine host or Moonlight binary is configured.'
            return False
        try:
            result = subprocess.run(
                [binary_path + 'moonlight', 'unpair', address],
                cwd=binary_path,
                env=env_with_libs(binary_path, self.config_helper.addon_path),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=10, encoding='utf-8', start_new_session=True)
            output = (result.stdout or '').strip()
        except (OSError, subprocess.TimeoutExpired) as error:
            self.last_unpair_error = 'Unable to unpair from %s: %s' % (address, error)
            return False

        # Transport/protocol errors from the native client are authoritative.
        # An unauthenticated HTTP PairStatus=0 must NEVER override a failed
        # /unpair response: public serverinfo is not a trusted pairing result.
        if result.returncode != 0:
            self.last_unpair_error = (
                'Native Moonlight unpair failed (exit %s) for %s: %s'
                % (result.returncode, address,
                   output or 'No error details returned'))
            return False

        # Native /unpair may succeed even when a legacy client has no trusted
        # server certificate. Never mistake missing trust for a TLS mismatch:
        # report the accepted request as UNVERIFIED, not as confirmed remote
        # unpairing. A saved pin still requires authenticated verification.
        try:
            from resources.lib.util.hostapi import Host
            host = Host(address)
            if not host.has_trusted_pin():
                self.last_unpair_unverified = True
                return True
            if not host.pair_status(allow_http_fallback=True):
                # A public HTTP 0 is useful evidence after client revocation,
                # not an authenticated server assertion.
                self.last_unpair_unverified = host._last_pair_status_public
                return True
        except Exception as error:
            self.last_unpair_error = (
                'Cannot confirm that %s was unpaired: %s' % (address, error))
            return False
        self.last_unpair_error = (
            'Host %s still reports this device paired. %s' % (address, output))
        return False

    def list_games(self):
        self.config_helper.configure()
        binary_path = self.config_helper.binary_path
        list_regex = r'\d+\.\s+([^\n]*)'

        try:
            moonlightOut = subprocess.check_output([binary_path + 'moonlight', 'list'], cwd=binary_path, env=env_with_libs(binary_path, self.config_helper.addon_path), timeout=5, encoding='utf-8', start_new_session=True)

            if 'must pair' in moonlightOut:
                return True

            return re.findall(list_regex, moonlightOut)
        except:
            return False

    KEEPALIVE = 'audiooutput.streamsilence'

    def _kodi_setting(self, name, value=None):
        import json
        try:
            if value is None:
                request = {'jsonrpc': '2.0', 'id': 1,
                           'method': 'Settings.GetSettingValue',
                           'params': {'setting': name}}
                answer = json.loads(xbmc.executeJSONRPC(json.dumps(request)))
                return answer.get('result', {}).get('value')

            request = {'jsonrpc': '2.0', 'id': 1,
                       'method': 'Settings.SetSettingValue',
                       'params': {'setting': name, 'value': value}}
            json.loads(xbmc.executeJSONRPC(json.dumps(request)))
            return True
        except Exception:
            return None

    def _held_nodes(self):
        import glob
        import subprocess
        held = []
        for node in sorted(glob.glob('/dev/snd/pcmC0D*p')):
            try:
                out = subprocess.check_output(['fuser', node],
                                              stderr=subprocess.DEVNULL,
                                              encoding='utf-8')
                if out.strip():
                    held.append(node.rsplit('/', 1)[-1])
            except Exception:
                pass
        return held

    def release_audio(self):
        current = self._kodi_setting(self.KEEPALIVE)
        xbmc.log('LUNA audio: keepalive was %r, nodes held by %s'
                 % (current, self._held_nodes()), xbmc.LOGINFO)
        if current in (None, 0):
            return None

        self.plugin.setSettingString('saved_streamsilence', str(current))
        written = self._kodi_setting(self.KEEPALIVE, 0)
        after = self._kodi_setting(self.KEEPALIVE)
        xbmc.log('LUNA audio: wrote %r, now %r' % (written, after),
                 xbmc.LOGINFO)
        return current

    def wait_audio_free(self, seconds=6):
        import time
        deadline = time.time() + seconds
        while True:
            held = self._held_nodes()
            if not held:
                xbmc.log('LUNA audio: nodes are free', xbmc.LOGINFO)
                return True
            if time.time() >= deadline:
                xbmc.log('LUNA audio: still held: %s' % held, xbmc.LOGERROR)
                return False
            time.sleep(0.3)

    def restore_audio(self, previous):
        if previous is None:
            return
        self._kodi_setting(self.KEEPALIVE, previous)
        self.plugin.setSettingString('saved_streamsilence', '')

    def list_games_detailed(self):
        from resources.lib.util.hostapi import Host

        self.last_host_error = None
        address = self.config_helper.config.get('host_addr')
        if not address:
            self.last_host_error = 'No Sunshine host is configured.'
            return None

        try:
            apps = Host(address).applist()
        except Exception as error:
            self.last_host_error = str(error)
            xbmc.log('LUNA applications: %s: %s' % (address, error),
                     xbmc.LOGERROR)
            return None

        result = []
        for app_id, name in apps:
            try:
                art = self.boxart_path(address, app_id)
            except Exception:
                art = None
            result.append((app_id, name, art))
        return result

    @staticmethod
    def _host_cache_key(address):
        # Do not let host names, ports or IPv6 brackets become path segments.
        import hashlib
        normalized = (address or '').strip().lower()
        return hashlib.sha256(normalized.encode('utf-8')).hexdigest()[:20] if normalized else None

    def cached_games(self, max_age=300):
        import json
        import time
        import xbmcvfs

        self.last_host_error = None
        address = self.config_helper.config.get('host_addr')
        host_key = self._host_cache_key(address)
        if host_key is None:
            self.last_host_error = 'No Sunshine host is configured.'
            return None

        folder = xbmcvfs.translatePath(
            'special://profile/addon_data/script.luna/')
        path = os.path.join(folder, 'applist.json')
        fresh = False
        cached = None
        try:
            age = time.time() - os.path.getmtime(path)
            with open(path, encoding='utf-8') as f:
                payload = json.load(f)
            # Earlier cache lists did not identify a host. Never reuse them
            # after switching servers.
            if isinstance(payload, dict) and payload.get('host') == host_key:
                apps = payload.get('apps')
                if isinstance(apps, list) and all(
                        isinstance(item, list) and len(item) == 2 and
                        isinstance(item[0], str) and item[0].isascii() and
                        item[0].isdigit() and isinstance(item[1], str)
                        for item in apps):
                    cached = apps
                    fresh = 0 <= age < max_age
        except (OSError, ValueError, TypeError):
            pass

        def cached_items():
            return [(app_id, name, self._art_if_present(address, app_id))
                    for app_id, name in cached]

        if fresh and cached is not None:
            return cached_items()

        detailed = self.list_games_detailed()
        if detailed is None:
            return cached_items() if cached is not None else None

        # Persist the original cached host's legacy folder assignments before
        # overwriting the only cache that identifies their original owner.
        from resources.lib.util.library import Library
        if not Library(folder, host=address).migrate_legacy():
            self.last_host_error = 'Unable to migrate saved folders; application cache not replaced.'
            return detailed

        temporary = None
        try:
            os.makedirs(folder, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                    mode='w', encoding='utf-8', dir=folder,
                    prefix='.applist.', suffix='.tmp', delete=False) as stream:
                temporary = stream.name
                json.dump({'host': host_key,
                           'apps': [[str(a), n] for a, n, _ in detailed]}, stream)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        except OSError:
            pass
        finally:
            if temporary:
                try:
                    os.remove(temporary)
                except OSError:
                    pass
        return detailed

    @staticmethod
    def _complete_artwork(data):
        from resources.lib.util.artwork import valid_artwork
        return valid_artwork(data)

    @classmethod
    def _complete_artwork_file(cls, path):
        try:
            if (os.path.islink(path) or not os.path.isfile(path) or
                    os.path.getsize(path) > 16 * 1024 * 1024):
                return False
            from resources.lib.util.artwork import valid_artwork
            with open(path, 'rb') as stream:
                data = stream.read()
            if valid_artwork(data, for_cache=True):
                return True
            # Kodi has its own JPEG decoder, but Pillow isn't always
            # available in the embedded Python runtime. Without a decoder,
            # reuse a structurally valid JPEG for at most 15 minutes instead
            # of downloading the same image on every UI refresh. The short
            # TTL bounds how long a broken but plausible JPEG can persist.
            if data.startswith(b'\xff\xd8') and valid_artwork(data):
                return 0 <= time.time() - os.path.getmtime(path) < 900
            return False
        except OSError:
            return False

    def _art_if_present(self, address, app_id):
        import xbmcvfs
        if not str(app_id).isascii() or not str(app_id).isdigit():
            return None
        key = self._host_cache_key(address)
        if not key:
            return None
        folder = xbmcvfs.translatePath(
            'special://profile/addon_data/script.luna/boxart/')
        path = os.path.join(folder, key, '%s.png' % app_id)
        return path if self._complete_artwork_file(path) else None

    def boxart_path(self, address, app_id):
        import xbmcvfs
        from resources.lib.util.hostapi import Host

        if not str(app_id).isascii() or not str(app_id).isdigit():
            return None
        key = self._host_cache_key(address)
        if not key:
            return None
        root = xbmcvfs.translatePath(
            'special://profile/addon_data/script.luna/boxart/')
        folder = os.path.join(root, key)
        path = os.path.join(folder, '%s.png' % app_id)
        if self._complete_artwork_file(path):
            return path

        data = Host(address).boxart(app_id)
        if not self._complete_artwork(data) or len(data) > 16 * 1024 * 1024:
            return None

        temporary = None
        try:
            os.makedirs(folder, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                    mode='wb', dir=folder, prefix='.boxart.',
                    suffix='.tmp', delete=False) as stream:
                temporary = stream.name
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
            return path
        except OSError:
            return None
        finally:
            if temporary:
                try:
                    os.unlink(temporary)
                except FileNotFoundError:
                    pass

    def quit_game(self, host):
        # A verified Resume record contains the precise Sunshine destination.
        # Do not let a stale moonlight.conf or fresh autodiscovery select it.
        if (not isinstance(host, str) or not host.strip() or host.startswith('-') or
                any(char.isspace() for char in host)):
            self.logger.info('[MoonlightHelper] - refusing Quit without a valid host')
            return False
        binary_path = self.config_helper.binary_path
        if not binary_path:
            return False
        try:
            result = subprocess.run(
                [binary_path + 'moonlight', 'quit', host],
                cwd=binary_path,
                env=env_with_libs(binary_path, self.config_helper.addon_path),
                timeout=15, start_new_session=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        except (OSError, subprocess.TimeoutExpired) as error:
            self.logger.info('[MoonlightHelper] - quit request failed: %s' % error)
            return False

        if result.returncode != 0:
            self.logger.info('[MoonlightHelper] - quit request exited with %s: %s' %
                             (result.returncode, result.stderr.strip()))
            return False
        return True

    def _create_bug_report(self, scripts_path):
        command = [scripts_path + '../../create_bug_report.sh']
        try:
            process = subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                encoding='utf-8', cwd=scripts_path + '../../',
                start_new_session=True)
        except OSError as error:
            self.logger.info('[MoonlightHelper] - bug report did not start: %s' % error)
            return None

        try:
            output, _ = process.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            self.logger.info('[MoonlightHelper] - bug report timed out')
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            except OSError:
                pass
            process.communicate()
            return None

        if process.returncode != 0:
            self.logger.info('[MoonlightHelper] - bug report exited with %s'
                             % process.returncode)
            return None

        return output.strip() or None

    @staticmethod
    def _is_coreelec_ng(binary_path):
        normalized = os.path.normpath(binary_path or '')
        return os.path.basename(normalized) == 'coreelec-ng'

    def _stream_environment(self):
        env = moonlight_environment(self.config_helper)
        # The detached CoreELEC launcher must record which host confirmed
        # stream readiness, not merely the last application name.
        env['LUNA_RESUME_HOST'] = (self.config_helper.config.get('host_addr')
                                   or self.plugin.getSettingString('host_addr') or '')

        env['MOONLIGHT_STREAM_DIAGNOSTICS'] = (
            '1' if self.plugin.getSettingBool('debug_mode') else '0')

        if self.plugin.getSettingBool('aml_profile'):
            env['MOONLIGHT_AML_PROFILE'] = '1'
        else:
            env.pop('MOONLIGHT_AML_PROFILE', None)

        if self.plugin.getSettingBool('alsa_resync'):
            env['MOONLIGHT_ALSA_RESYNC'] = '1'
        else:
            env.pop('MOONLIGHT_ALSA_RESYNC', None)

        if self.plugin.getSettingBool('cpu_performance_governor'):
            env['MOONLIGHT_CPU_PERFORMANCE'] = '1'
        else:
            env.pop('MOONLIGHT_CPU_PERFORMANCE', None)

        for setting, variable in (
                ('input_handoff_probe', 'MOONLIGHT_INPUT_HANDOFF_PROBE'),
                ('input_recovery', 'MOONLIGHT_INPUT_RECOVERY'),
                ('aml_tuning_probe', 'MOONLIGHT_AML_TUNING_PROBE')):
            if self.plugin.getSettingBool(setting):
                env[variable] = '1'
            else:
                env.pop(variable, None)

        frame_pacing = self.plugin.getSettingString('frame_pacing') or 'latency'
        if frame_pacing not in ('latency', 'balanced'):
            frame_pacing = 'latency'
        # Pass the same stable device identities used to resolve the startup
        # config. Native Moonlight verifies every opened fd, then uses them
        # again for the initial udev scan and later reconnects.
        from resources.lib.util import inputdevices
        selected = inputdevices.load_selectors(self.plugin)
        env.pop('MOONLIGHT_INPUT_NAMES_HEX', None)
        if selected:
            env['MOONLIGHT_INPUT_SELECTORS_V2'] = inputdevices.encode_selectors(selected)
        else:
            env.pop('MOONLIGHT_INPUT_SELECTORS_V2', None)

        env['MOONLIGHT_FRAME_PACING'] = frame_pacing
        env['MOONLIGHT_TV_GAME_MODE'] = '1' if self.plugin.getSettingBool('tv_game_mode') else '0'

        pipeline = self.plugin.getSettingString('video_pipeline') or 'aml'
        if pipeline not in ('aml', 'aml-vfm'):
            pipeline = 'aml'
        env['MOONLIGHT_VIDEO_PIPELINE'] = pipeline
        env.pop('MOONLIGHT_VFM_ROOT', None)
        if pipeline == 'aml-vfm':
            root = os.path.join(self.config_helper.addon_path, 'vfm')
            env['MOONLIGHT_VFM_ROOT'] = root
            previous = env.get('LD_LIBRARY_PATH', '')
            env['LD_LIBRARY_PATH'] = os.path.join(root, 'lib') + (':' + previous if previous else '')

        env['MOONLIGHT_BINARY'] = self.config_helper.binary_path + 'moonlight'
        return env

    def _preflight_coreelec_ng_host(self, binary_path, env):
        """Verify Sunshine is responsive before Kodi is stopped."""
        command = [binary_path + 'moonlight', 'list']
        if env.get('LUNA_STREAM_CONFIG'):
            command += ['-config', env['LUNA_STREAM_CONFIG']]
        try:
            result = subprocess.run(
                command,
                cwd=binary_path,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                encoding='utf-8',
                errors='replace',
                timeout=12,
                check=False)
        except subprocess.TimeoutExpired:
            return False, (
                'Sunshine did not respond within 12 seconds. '
                'Kodi was left running; restart Sunshine and try again.')
        except OSError as error:
            return False, 'Moonlight host preflight failed: %s' % error

        if result.returncode != 0:
            output = (result.stdout or '').strip()
            return False, output or (
                'Moonlight could not verify the Sunshine connection. '
                'Kodi was left running.')

        return True, None

    def _launch_coreelec_ng_service(self, game_id, scripts_path, env):
        systemd_run = '/usr/bin/systemd-run'
        if not os.path.isfile(systemd_run):
            systemd_run = 'systemd-run'

        forwarded = {}
        for key, value in env.items():
            if (key in ('HOME', 'PATH', 'LD_LIBRARY_PATH',
                        'XDG_CACHE_DIR', 'XDG_CONFIG_DIR') or
                    key.startswith('MOONLIGHT_') or key.startswith('LUNA_')):
                forwarded[key] = value

        # Forward only this explicitly supported SDL override, not the
        # Kodi process's full environment.
        mapping = env.get('SDL_GAMECONTROLLERCONFIG')
        if mapping:
            if (not isinstance(mapping, str) or '\x00' in mapping or
                    len(mapping.encode('utf-8')) > 16384):
                return False, 'SDL controller mapping override exceeds 16 KiB or is invalid.'
            forwarded['SDL_GAMECONTROLLERCONFIG'] = mapping

        unit_name = 'luna-ng-session-' + uuid.uuid4().hex[:12]
        unit = unit_name + '.service'
        forwarded['LUNA_SESSION_UNIT'] = unit

        # This receipt is created before submission. The independent launcher
        # writes 'entered' only after it executes and acquires its launch lock;
        # systemd-run --no-block returning 0 proves only that a job was queued.
        snapshot = env.get('LUNA_STREAM_CONFIG')
        ack_path = None
        if snapshot:
            if not os.path.isfile(snapshot):
                return False, 'Private Luna stream configuration is missing.'
            try:
                ack_fd, ack_path = tempfile.mkstemp(
                    prefix='.luna-ack-', suffix='.ack',
                    dir=os.path.dirname(snapshot))
                os.close(ack_fd)
            except OSError as error:
                return False, 'Unable to create Luna launcher acknowledgment: %s' % error
            forwarded['LUNA_LAUNCH_ACK'] = ack_path
            forwarded['LUNA_ACK_REQUIRED'] = '1'

        command = [
            systemd_run,
            '--unit=' + unit_name,
            '--property=TimeoutStopSec=30s',
            '--property=SendSIGKILL=yes',
            '--property=KillMode=control-group',
            '--collect', '--quiet', '--no-block',
            '/usr/bin/env',
        ]
        command.extend('%s=%s' % item for item in sorted(forwarded.items()))
        command.extend([scripts_path + 'launch.sh', game_id])

        retain_ack = False
        try:
            try:
                result = subprocess.run(
                    command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    encoding='utf-8',
                    errors='replace',
                    timeout=10,
                    check=False)
            except subprocess.TimeoutExpired:
                # A timed-out CLI may still have queued the unit. Do not
                # delete the stream configuration before stopping it.
                result = None
            except OSError as error:
                xbmc.log('LUNA NG systemd handoff failed: %s' % error,
                         xbmc.LOGERROR)
                return False, str(error)

            if result is not None and result.returncode != 0:
                message = (result.stdout or '').strip()
                xbmc.log('LUNA NG systemd handoff failed (%d): %s'
                         % (result.returncode, message), xbmc.LOGERROR)
                return False, message or 'Unable to queue Luna NG session.'

            if not ack_path:
                # Compatibility for callers that do not request a detached
                # session snapshot. The normal CoreELEC NG path always does.
                return True, None

            start = time.monotonic()
            deadline = start + LUNA_NG_ACK_WAIT_SECONDS
            final_deadline = start + LUNA_NG_RECOVERY_MAX_SECONDS
            last_step = None
            reconciling = False
            while time.monotonic() <= deadline:
                try:
                    with open(ack_path, encoding='ascii') as receipt:
                        phase = receipt.read(64).strip()
                    if phase == 'armed':
                        # Kodi must confirm seeing 'armed' before the
                        # detached launcher is allowed to stop kodi.bin.
                        try:
                            with open(ack_path, 'r+', encoding='ascii') as receipt:
                                if receipt.read(64).strip() != 'armed':
                                    continue
                                receipt.seek(0)
                                receipt.write('observed\n')
                                receipt.truncate()
                                receipt.flush()
                                os.fsync(receipt.fileno())
                        except OSError as error:
                            retain_ack = True
                            return None, ('Cannot confirm Luna handoff: %s. '
                                          'The detached unit retains ownership.'
                                          % error)
                        retain_ack = True
                        return True, None
                    if phase == 'failed':
                        return False, ('Luna launcher exited before recovery '
                                       'was armed. Kodi was left running.')
                    if re.fullmatch(r'reconciling:[1-9][0-9]{0,3}', phase):
                        reconciling = True
                        if phase != last_step:
                            last_step = phase
                            deadline = min(
                                time.monotonic() + LUNA_NG_RECOVERY_STEP_SECONDS,
                                final_deadline)
                except OSError:
                    pass
                time.sleep(0.1)

            if reconciling:
                # Leaving the wait loop withdraws permission to stop Kodi.
                # Do not interrupt the older recovery, but explicitly cancel
                # this new stream request so a late arm cannot succeed.
                try:
                    with open(ack_path, 'r+', encoding='ascii') as receipt:
                        latest = receipt.read(64).strip()
                        if latest == 'armed':
                            receipt.seek(0)
                            receipt.write('observed\n')
                            receipt.truncate()
                            receipt.flush()
                            os.fsync(receipt.fileno())
                            retain_ack = True
                            return True, None
                        if latest == 'failed':
                            return False, ('Luna launcher rejected the new handoff '
                                           'without stopping Kodi.')
                        # A sidecar is never replaced by the launcher's
                        # atomic reconciling/armed receipt updates.
                        abandoned_fd = os.open(
                            ack_path + '.abandoned',
                            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                            0o600)
                        with os.fdopen(abandoned_fd, 'w', encoding='ascii') as marker:
                            marker.write('abandoned\n')
                            marker.flush()
                            os.fsync(marker.fileno())
                        receipt.seek(0)
                        receipt.write('abandoned\n')
                        receipt.truncate()
                        receipt.flush()
                        os.fsync(receipt.fileno())
                except OSError as error:
                    xbmc.log('LUNA NG cannot publish handoff abandonment: %s' %
                             error, xbmc.LOGERROR)
                retain_ack = True
                return None, ('Earlier recovery continues, but this new handoff '
                              'was abandoned before Kodi stopped. '
                              'Retry after recovery finishes.')

            # Missing acknowledgment: cancel ONLY the unique transient unit.
            # This is not proof of failure until systemd confirms it stopped.
            control = env.get('LUNA_SYSTEMCTL') or 'systemctl'
            try:
                stopped = subprocess.run(
                    [control, 'stop', unit],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    timeout=10, check=False)
                state = subprocess.run(
                    [control, 'show', '--property=ActiveState', '--value', unit],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    encoding='utf-8', errors='replace', timeout=5, check=False)
                if (stopped.returncode == 0 and state.returncode == 0 and
                        state.stdout.strip() in ('inactive', 'failed')):
                    return False, ('Luna service did not acknowledge startup; '
                                   'the transient unit was stopped without '
                                   'taking Kodi away.')
            except (OSError, subprocess.TimeoutExpired) as error:
                xbmc.log('LUNA NG cannot confirm failed service startup: %s'
                         % error, xbmc.LOGERROR)

            # When the service state cannot be confirmed, the launcher may
            # still arrive late. Keep its snapshot/receipt for that process.
            retain_ack = True
            return None, ('Luna launch was submitted but not acknowledged. '
                          'Could not confirm the service stopped; its private '
                          'configuration was retained for safe recovery.')
        finally:
            if ack_path and not retain_ack:
                try:
                    os.unlink(ack_path)
                except OSError:
                    pass

    def launch_game(self, game_id, resume=False):
        self.config_helper.configure()

        binary_path = self.config_helper.binary_path
        scripts_path = self.config_helper.launchscripts_path
        if not binary_path:
            message = ('Moonlight executable was not found or failed architecture validation. '
                       'Check the Kodi log for each candidate path and ELF class.')
            xbmc.log('LUNA NG launch aborted: %s (addon=%s)' %
                     (message, self.config_helper.addon_path), xbmc.LOGERROR)
            xbmcgui.Dialog().ok('Luna NG executable unavailable', message)
            return

        # Check installed ELF hashes and run Moonlight's help action while
        # Kodi is still responsive. A missing library must not freeze the TV.
        try:
            from resources.lib.util.runtimecheck import (
                check_ng_runtime, NativeRuntimeError)
            check_ng_runtime(self.config_helper.addon_path,
                             binary_path + 'moonlight',
                             moonlight_environment(self.config_helper))
        except NativeRuntimeError as error:
            xbmc.log('LUNA NG runtime preflight: %s' % error, xbmc.LOGERROR)
            xbmcgui.Dialog().ok(
                'Luna NG runtime check failed',
                str(error))
            return
        except Exception:
            xbmc.log('LUNA NG unexpected runtime preflight exception:\n%s' %
                     traceback.format_exc(), xbmc.LOGERROR)
            xbmcgui.Dialog().ok(
                'Luna NG runtime check failed',
                'Unable to verify Moonlight before launch. See the Kodi log.')
            return

        env = self._stream_environment()
        if env['MOONLIGHT_VIDEO_PIPELINE'] == 'aml-vfm':
            from resources.lib.util.vfmruntime import check_runtime, VfmRuntimeError
            try:
                if (not self._is_coreelec_ng(binary_path) or
                        os.path.basename(os.path.normpath(scripts_path)) != 'coreelec'):
                    raise VfmRuntimeError('The prototype requires the CoreELEC NG launcher.')
                if self.plugin.getSettingBool('hdr_flag') or self.plugin.getSettingBool('osd'):
                    raise VfmRuntimeError('Disable HDR and the statistics overlay for this SDR prototype.')
                check_runtime(self.config_helper.addon_path, binary_path + 'moonlight', env)
            except VfmRuntimeError as error:
                xbmcgui.Dialog().ok('Experimental video pipeline unavailable', str(error))
                return

        player = xbmc.Player()
        if player.isPlaying():
            player.stop()
            deadline = time.time() + 5
            while player.isPlaying() and time.time() < deadline:
                time.sleep(0.1)

        isResumeMode = bool(resume)
        showIntro = self.plugin.getSettingBool('show_intro')
        codec = self.config_helper.config['codec']

        xbmc.executebuiltin("Dialog.Close(busydialog)")
        xbmc.executebuiltin("Dialog.Close(notification)")
        xbmc.executebuiltin("InhibitScreensaver(true)")

        keepalive = None
        t0 = None
        try:
            if showIntro and not isResumeMode:
                player.play(self.config_helper.addon_path + 'resources/statics/loading.mp4')
                time.sleep(10)
                player.stop()

            if self._is_coreelec_ng(binary_path):
                snapshot = None
                try:
                    # Do not read shared moonlight.conf again after
                    # preflight: another Kodi action may change it while
                    # systemd is starting the detached session.
                    snapshot = self.config_helper.create_launch_snapshot()
                    env['LUNA_STREAM_CONFIG'] = snapshot
                    ready, error = self._preflight_coreelec_ng_host(
                        binary_path, env)
                    if not ready:
                        xbmc.log('LUNA NG Sunshine preflight failed: %s' % error,
                                 xbmc.LOGERROR)
                        xbmcgui.Dialog().ok(
                            'Luna NG host unavailable',
                            error or 'Sunshine did not respond.')
                        return

                    launched, error = self._launch_coreelec_ng_service(
                        game_id, scripts_path, env)
                    if launched is False:
                        xbmcgui.Dialog().ok(
                            'Luna NG launch failed',
                            error or 'Unable to hand the session to systemd.')
                    elif launched is None:
                        # A queued unit with uncertain ownership must keep
                        # the snapshot until it is safely retired.
                        snapshot = None
                        xbmcgui.Dialog().ok(
                            'Luna NG launch status unconfirmed', error)
                    else:
                        # The independent session owns deleting this snapshot.
                        snapshot = None
                    return
                finally:
                    if snapshot:
                        try:
                            os.unlink(snapshot)
                        except OSError:
                            pass

            keepalive = self.release_audio()
            xbmc.audioSuspend()

            free = self.wait_audio_free(2)
            if not free:
                xbmc.log('LUNA audio: node taken again, retrying',
                         xbmc.LOGINFO)
                xbmc.audioSuspend()
                free = self.wait_audio_free()

            if not free:
                held = ', '.join(self._held_nodes())
                xbmc.log('LUNA audio: launch cancelled, held by %s' % held,
                         xbmc.LOGERROR)
                xbmcgui.Dialog().ok(self.plugin.getLocalizedString(30037),
                                    self.plugin.getLocalizedString(30268))
                return

            t0 = time.monotonic()
            subprocess.run([scripts_path + 'prescript.sh', binary_path, codec], cwd=scripts_path, start_new_session=True)
            env = self._stream_environment()
            launch_cmd = subprocess.Popen([scripts_path + 'launch.sh', game_id], cwd=binary_path, env=env, start_new_session=True)
            launch_status = launch_cmd.wait()
            if launch_status == 0 and not isResumeMode:
                from resources.lib.util.resumestate import host_identity
                self.plugin.setSettingString('last_run', game_id)
                self.plugin.setSettingString('last_run_host', host_identity(
                    self.config_helper.config.get('host_addr')
                    or self.plugin.getSettingString('host_addr')))
            subprocess.run([scripts_path + 'postscript.sh', binary_path], cwd=scripts_path, start_new_session=True)

        except Exception:
            xbmc.log('LUNA NG launch exception:\n%s' % traceback.format_exc(),
                     xbmc.LOGERROR)
            xbmcgui.Dialog().ok(
                'Luna NG launch failed',
                'Moonlight could not be launched. See the Kodi log for the error details.')
        finally:
            xbmc.audioResume()
            self.restore_audio(keepalive)
            xbmc.executebuiltin("InhibitScreensaver(false)")

            if t0 is None:
                pass
            elif time.monotonic() - t0 < 10:
                result = xbmcgui.Dialog().yesno(
                    "", self.plugin.getLocalizedString(30288))
                if result:
                    url = self._create_bug_report(scripts_path)
                    if url:
                        xbmcgui.Dialog().ok(
                            "", self.plugin.getLocalizedString(30289) + ' ' + url)
                    else:
                        xbmcgui.Dialog().ok(xbmc.getLocalizedString(257),
                                            self.plugin.getLocalizedString(30221))
            else:
                xbmcgui.Dialog().notification(
                    self.plugin.getLocalizedString(30290),
                    self.plugin.getLocalizedString(30291) % game_id,
                    xbmcgui.NOTIFICATION_INFO, False)
