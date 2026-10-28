import os
import signal
import subprocess
import queue
import time
import re
import threading

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
        self.paired_address = None
        self.pairing_messages = ""
        self.pair_success = False
        self.last_host_error = None
        self.last_unpair_error = None

    def _pair_line(self, line, dialog):
        text = line.strip()
        if not text:
            return

        xbmc.log('LUNA pairing: ' + text, xbmc.LOGINFO)

        found = re.search(r'Connecting to (\S+?)\.\.\.', line)
        if found:
            self.paired_address = found.group(1)

        if 'enter the following PIN' in line:
            self.pin_received = True
            dialog.update(50, text)
            return

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
        self.paired_address = None
        self.pairing_messages = ""

        title = self.plugin.getLocalizedString(30000)
        if not address:
            xbmcgui.Dialog().ok(
                title, 'Set the Sunshine host address before pairing.')
            return False

        try:
            from resources.lib.util.hostapi import Host
            if Host(address).paired():
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
                    pass

            if cancelled:
                return False

            if timeout_message:
                xbmcgui.Dialog().ok(title, timeout_message)
                return False

            if ('Succesfully paired' in self.pairing_messages
                    or 'Successfully paired' in self.pairing_messages):
                self.pair_success = True
                self.paired_address = self.paired_address or address
                xbmcgui.Dialog().ok(
                    title, self.plugin.getLocalizedString(30286))
                return True

            message = self.pairing_messages.strip()
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

    def unpair(self):
        self.config_helper.configure()
        binary_path = self.config_helper.binary_path
        address = self.config_helper.config.get('host_addr')
        self.last_unpair_error = None

        if not address:
            self.last_unpair_error = 'No Sunshine host is configured.'
            return False

        try:
            result = subprocess.run(
                [binary_path + 'moonlight', 'unpair'],
                cwd=binary_path,
                env=env_with_libs(binary_path, self.config_helper.addon_path),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=10,
                encoding='utf-8',
                start_new_session=True)
            output = (result.stdout or '').strip()
        except subprocess.TimeoutExpired:
            self.last_unpair_error = 'Timed out while unpairing from %s.' % address
            return False
        except Exception as error:
            self.last_unpair_error = str(error)
            return False

        if ('Succesfully unpaired' in output
                or 'Successfully unpaired' in output):
            return True

        # moonlight-embedded historically exits zero for some unpair failures,
        # so verify the host state before reporting failure.
        try:
            from resources.lib.util.hostapi import Host
            if not Host(address).pair_status():
                return True
        except Exception:
            pass

        self.last_unpair_error = output or 'Unable to unpair from %s.' % address
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

    def cached_games(self, max_age=300):
        import json
        import time
        import xbmcvfs

        self.last_host_error = None
        folder = xbmcvfs.translatePath(
            'special://profile/addon_data/script.luna/')
        path = os.path.join(folder, 'applist.json')

        fresh = False
        cached = None
        try:
            age = time.time() - os.path.getmtime(path)
            with open(path, encoding='utf-8') as f:
                cached = json.load(f)
            fresh = age < max_age
        except (OSError, ValueError):
            pass

        if fresh and cached:
            return [(app_id, name, self._art_if_present(app_id))
                    for app_id, name in cached]

        detailed = self.list_games_detailed()
        if detailed is None:
            if cached:
                return [(app_id, name, self._art_if_present(app_id))
                        for app_id, name in cached]
            return None

        try:
            os.makedirs(folder, exist_ok=True)
            with open(path, 'w', encoding='utf-8') as f:
                json.dump([[a, n] for a, n, _ in detailed], f)
        except OSError:
            pass
        return detailed

    def _art_if_present(self, app_id):
        import xbmcvfs
        path = xbmcvfs.translatePath(
            'special://profile/addon_data/script.luna/boxart/%s.png' % app_id)
        return path if os.path.exists(path) else None

    def boxart_path(self, address, app_id):
        import xbmcvfs
        from resources.lib.util.hostapi import Host

        folder = xbmcvfs.translatePath(
            'special://profile/addon_data/script.luna/boxart/')
        path = os.path.join(folder, '%s.png' % app_id)
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return path

        data = Host(address).boxart(app_id)
        if not data:
            return None

        try:
            os.makedirs(folder, exist_ok=True)
            with open(path, 'wb') as f:
                f.write(data)
        except OSError:
            return None
        return path

    def quit_game(self):
        binary_path = self.config_helper.binary_path
        try:
            subprocess.run([binary_path + 'moonlight', 'quit'], cwd=binary_path, env=env_with_libs(binary_path, self.config_helper.addon_path), timeout=5, start_new_session=True)
        except:
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

        env['MOONLIGHT_STREAM_DIAGNOSTICS'] = (
            '1' if self.plugin.getSettingBool('stream_diagnostics') else '0')

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
        forwarded['LUNA_SESSION_UNIT'] = 'luna-ng-session.service'

        command = [
            systemd_run,
            '--unit=luna-ng-session',
            '--collect',
            '--quiet',
            '--no-block',
            '/usr/bin/env',
        ]
        command.extend('%s=%s' % item for item in sorted(forwarded.items()))
        command.extend([scripts_path + 'launch.sh', game_id])

        try:
            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                encoding='utf-8',
                errors='replace',
                timeout=10,
                check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            xbmc.log('LUNA NG systemd handoff failed: %s' % error,
                     xbmc.LOGERROR)
            return False, str(error)

        if result.returncode != 0:
            message = (result.stdout or '').strip()
            xbmc.log('LUNA NG systemd handoff failed (%d): %s'
                     % (result.returncode, message), xbmc.LOGERROR)
            return False, message or 'Unable to start Luna NG session service.'

        return True, None

    def launch_game(self, game_id):
        self.config_helper.configure()

        binary_path = self.config_helper.binary_path
        scripts_path = self.config_helper.launchscripts_path

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

        isResumeMode = bool(self.plugin.getSetting('last_run'))
        showIntro = self.plugin.getSettingBool('show_intro')
        codec = self.config_helper.config['codec']

        if not isResumeMode:
            self.plugin.setSettingString('last_run', game_id)

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
                if not launched:
                    xbmcgui.Dialog().ok(
                        'Luna NG launch failed',
                        error or 'Unable to hand the session to systemd.')
                return

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
            launch_cmd.wait()
            subprocess.run([scripts_path + 'postscript.sh', binary_path], cwd=scripts_path, start_new_session=True)

        except Exception as e:
            print("Failed to execute moonlight process.")
            print(e)
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
