import os
import stat
import subprocess

import sys
import urllib.parse
import re

from xml.etree import ElementTree

import xbmc
import xbmcaddon
import xbmcgui
import xbmcplugin
from resources.lib.util.confighelper import ConfigHelper
from resources.lib.util.inputdevices import InvalidInputSelection
from resources.lib.core.logger import Logger 
from xbmcvfs import translatePath

addon = xbmcaddon.Addon()
addon_path = addon.getAddonInfo('path')

base_url = sys.argv[0]
addon_handle = int(sys.argv[1])
args = urllib.parse.parse_qs(sys.argv[2][1:])

logger = Logger()
config_helper = ConfigHelper(addon, logger)

def getAddonPath(path):
    return translatePath(addon_path + path)

def ensurePermissions():
    targets = [getAddonPath('/bin/coreelec-ng/moonlight'),
               getAddonPath('/bin/coreelec-no/moonlight'),
               getAddonPath('/bin/moonlight'),
               getAddonPath('/resources/create_bug_report.sh')]

    for root, _dirs, files in os.walk(getAddonPath('/resources/launchscripts')):
        targets += [os.path.join(root, f) for f in files if f.endswith('.sh')]

    for path in targets:
        try:
            mode = os.stat(path).st_mode
            if not mode & stat.S_IXUSR:
                os.chmod(path, mode | 0o111)
        except OSError:
            pass


def _setPairingStatus(paired, label):
    persistSetting('pair_status_display', label)
    persistBoolSetting('paired_state', paired)


def syncPairingStatus():
    config_helper.configure(dump=False)
    address = addon.getSetting('host_addr') or config_helper.config.get('host_addr')

    if not address:
        _setPairingStatus(False, addon.getLocalizedString(30324) or 'Not configured')
        return False

    try:
        from resources.lib.util.hostapi import Host
        host = Host(address)
        if not host.has_pairing_keys():
            _setPairingStatus(False, addon.getLocalizedString(30322) or 'Not paired')
            return False
        # A host without an established identity pin cannot be called paired.
        if not host.has_trusted_pin():
            _setPairingStatus(False, 'Re-pair to verify Sunshine host identity')
            return False
        paired = host.pair_status()
    except Exception as error:
        logger.info('Pairing status unavailable for %s: %s' % (address, error))
        _setPairingStatus(False, addon.getLocalizedString(30323) or 'Host unavailable')
        return False

    _setPairingStatus(
        paired,
        (addon.getLocalizedString(30321) or 'Paired ✓') if paired
        else (addon.getLocalizedString(30322) or 'Not paired'))
    return paired


def settings():
    syncPairingStatus()
    addon.openSettings()
    config_helper.configure()

def launchByName():
    from resources.lib.util.moonlighthelper import MoonlightHelper

    name = args.get('game', [None])[0]
    if not name:
        return

    helper = MoonlightHelper(addon, config_helper, logger)
    helper.launch_game(name)
    xbmc.executebuiltin('Container.Refresh')
    del helper

def refreshGames():
    import shutil
    from xbmcvfs import translatePath
    if not _library().migrate_legacy():
        logger.error('Unable to migrate legacy folders; refresh cancelled')
        return

    folder = translatePath('special://profile/addon_data/script.luna/')
    try:
        os.remove(os.path.join(folder, 'applist.json'))
    except OSError:
        pass
    try:
        shutil.rmtree(os.path.join(folder, 'boxart'))
    except OSError:
        pass

    xbmc.executebuiltin('Container.Refresh')

def _library():
    from resources.lib.util.library import Library
    from xbmcvfs import translatePath
    address = addon.getSetting('host_addr') or config_helper.config.get('host_addr')
    return Library(translatePath('special://profile/addon_data/script.luna/'), host=address)

def _games():
    from resources.lib.util.moonlighthelper import MoonlightHelper
    helper = MoonlightHelper(addon, config_helper, logger)
    try:
        apps = helper.cached_games() or []
        return apps, helper.last_host_error
    except Exception as error:
        logger.error('Unable to load Sunshine applications: %s' % error)
        return [], str(error)
    finally:
        del helper


def _clearApplicationListCache():
    if not _library().migrate_legacy():
        logger.error('Unable to preserve legacy folder ownership; leaving applications cache intact')
        return
    folder = translatePath('special://profile/addon_data/%s/'
                           % addon.getAddonInfo('id'))
    try:
        os.remove(os.path.join(folder, 'applist.json'))
    except OSError:
        pass


def retryApplications():
    _clearApplicationListCache()
    xbmc.executebuiltin('Container.Refresh')


def _addApplicationRecoveryItems():
    li = xbmcgui.ListItem(addon.getLocalizedString(30329) or 'Retry')
    li.setArt({'icon': getAddonPath('/resources/icons/refresh.png')})
    xbmcplugin.addDirectoryItem(
        addon_handle, base_url + '?action=retry_apps', li, False)

    li = xbmcgui.ListItem(
        addon.getLocalizedString(30330) or 'Pair / Re-pair Host')
    li.setArt({'icon': getAddonPath('/resources/icons/controller.png')})
    xbmcplugin.addDirectoryItem(
        addon_handle, base_url + '?action=pair_from_apps', li, False)

def _appItem(app_id, name, art, folder):
    li = xbmcgui.ListItem(name)
    if art:
        li.setArt({'icon': art, 'thumb': art, 'poster': art})

    move = base_url + '?action=assign&id=' + urllib.parse.quote(app_id)
    if folder:
        move += '&folder=' + urllib.parse.quote(folder)
    li.addContextMenuItems([((addon.getLocalizedString(30253) or 'Move to folder...'), 'RunPlugin(%s)' % move)])
    return li

def showApps(folder=None):
    lib = _library()
    # Migrate while the old host-tagged cache still identifies its owner.
    if not lib.migrate_legacy():
        xbmcgui.Dialog().notification(
            addon.getLocalizedString(30000),
            'Unable to safely migrate saved folders; application refresh cancelled.',
            time=5000)
        xbmcplugin.endOfDirectory(addon_handle, succeeded=False)
        return
    apps, host_error = _games()
    address = addon.getSetting('host_addr') or config_helper.config.get('host_addr')

    if host_error:
        host_label = address or (addon.getLocalizedString(30324)
                                 or 'Not configured')
        message = ((addon.getLocalizedString(30327)
                    or 'Unable to retrieve applications from %s.')
                   % host_label)
        if apps:
            message += ' ' + (addon.getLocalizedString(30328)
                              or 'Showing cached applications.')
            xbmcgui.Dialog().notification(
                addon.getLocalizedString(30000), message, time=5000)
        else:
            xbmcgui.Dialog().ok(addon.getLocalizedString(30000), message)
        _addApplicationRecoveryItems()
        if not apps:
            xbmcplugin.endOfDirectory(addon_handle)
            return

    folders, loose = lib.split(apps)

    if folder:
        xbmcplugin.setContent(addon_handle, 'games')
        for app_id, name, art in dict(folders).get(folder, []):
            url = base_url + '?action=launch&game=' + urllib.parse.quote(name)
            xbmcplugin.addDirectoryItem(addon_handle, url,
                                        _appItem(app_id, name, art, folder), False)
        xbmcplugin.endOfDirectory(addon_handle)
        return

    for name, inside in folders:
        li = xbmcgui.ListItem('%s (%d)' % (name, len(inside)))
        li.setArt({'icon': 'DefaultFolder.png'})
        rename = base_url + '?action=renfolder&folder=' + urllib.parse.quote(name)
        delete = base_url + '?action=delfolder&folder=' + urllib.parse.quote(name)
        li.addContextMenuItems([((addon.getLocalizedString(30256) or 'Rename folder'), 'RunPlugin(%s)' % rename),
                                ((addon.getLocalizedString(30257) or 'Delete folder'), 'RunPlugin(%s)' % delete)])
        url = base_url + '?action=apps&folder=' + urllib.parse.quote(name)
        xbmcplugin.addDirectoryItem(addon_handle, url, li, True)

    li = xbmcgui.ListItem((addon.getLocalizedString(30251) or 'New folder...'))
    li.setArt({'icon': 'DefaultAddSource.png'})
    xbmcplugin.addDirectoryItem(addon_handle, base_url + '?action=newfolder', li, False)

    li = xbmcgui.ListItem(addon.getLocalizedString(30249) or 'Refresh list')
    li.setArt({'icon': getAddonPath('/resources/icons/refresh.png')})
    xbmcplugin.addDirectoryItem(addon_handle, base_url + '?action=refresh', li, False)

    if loose:
        xbmcplugin.setContent(addon_handle, 'games')
    for app_id, name, art in loose:
        url = base_url + '?action=launch&game=' + urllib.parse.quote(name)
        xbmcplugin.addDirectoryItem(addon_handle, url,
                                    _appItem(app_id, name, art, None), False)

    xbmcplugin.endOfDirectory(addon_handle)

def newFolder():
    name = xbmcgui.Dialog().input((addon.getLocalizedString(30252) or 'New folder name'))
    if name:
        _library().add_folder(name)

def renameFolder():
    old = args.get('folder', [None])[0]
    if not old:
        return
    new = xbmcgui.Dialog().input((addon.getLocalizedString(30256) or 'Rename folder'), defaultt=old)
    if new and new != old:
        _library().rename_folder(old, new)
    xbmc.executebuiltin('Container.Refresh')

def deleteFolder():
    name = args.get('folder', [None])[0]
    if not name:
        return
    confirmed = xbmcgui.Dialog().yesno(
        (addon.getLocalizedString(30257) or 'Delete folder'),
        (addon.getLocalizedString(30258)
         or 'Delete "%s"?\nApplications will move back to the unsorted list.') % name,
        nolabel=xbmc.getLocalizedString(106), yeslabel=xbmc.getLocalizedString(107))
    if confirmed:
        _library().remove_folder(name)
    xbmc.executebuiltin('Container.Refresh')

def assignApp():
    app_id = args.get('id', [None])[0]
    if not app_id:
        return

    lib = _library()
    folders = lib.folders()
    current = lib.folder_of(app_id)

    targets = [f for f in folders if f != current]

    choices = [(addon.getLocalizedString(30251) or 'New folder...')]
    if current:
        choices.append((addon.getLocalizedString(30255) or 'Remove from "%s"') % current)
    choices += targets

    picked = xbmcgui.Dialog().select((addon.getLocalizedString(30254) or 'Move to folder'), choices)
    if picked == -1:
        return

    if picked == 0:
        name = xbmcgui.Dialog().input((addon.getLocalizedString(30252) or 'New folder name'))
        if name and lib.add_folder(name):
            lib.assign(app_id, name.strip())
    elif current and picked == 1:
        lib.assign(app_id, None)
    else:
        lib.assign(app_id, targets[picked - (2 if current else 1)])

    xbmc.executebuiltin('Container.Refresh')

def _active_resume_session():
    """Return the app and its *specific* confirmed host as one guarded value."""
    from resources.lib.util.library import Library
    address = (addon.getSetting('host_addr') or
               config_helper.config.get('host_addr') or '').strip()
    if not address or addon.getSetting('last_run_host') != Library.host_identity(address):
        return None
    app = addon.getSetting('last_run')
    return (app, address) if app else None


def _active_resume_app():
    session = _active_resume_session()
    return session[0] if session else None


def main():
    li = xbmcgui.ListItem((addon.getLocalizedString(30250) or 'Applications'))
    li.setArt({ 'icon': getAddonPath('/resources/icons/apps.png') })
    xbmcplugin.addDirectoryItem(addon_handle, base_url + '?action=apps', li, True)

    li = xbmcgui.ListItem(addon.getLocalizedString(30259)
                          or 'List applications')
    li.setArt({ 'icon': getAddonPath('/resources/icons/list.png') })
    xbmcplugin.addDirectoryItem(addon_handle, base_url + '?action=quick_play&refresh=True', li)

    lastrun = _active_resume_app()
    if lastrun:
        li = xbmcgui.ListItem((addon.getLocalizedString(30260) or 'Resume %s') % lastrun)
        li.setArt({ 'icon': getAddonPath('/resources/icons/resume.png') })
        xbmcplugin.addDirectoryItem(addon_handle, base_url + '?action=resume', li)

        li = xbmcgui.ListItem((addon.getLocalizedString(30261) or 'Quit %s') % lastrun)
        li.setArt({ 'icon': getAddonPath('/resources/icons/quit.png') })
        xbmcplugin.addDirectoryItem(addon_handle, base_url + '?action=quit', li)

    li = xbmcgui.ListItem(xbmc.getLocalizedString(5))
    li.setArt({ 'icon': getAddonPath('/resources/icons/settings.png') })
    xbmcplugin.addDirectoryItem(addon_handle, base_url + '?action=settings', li)

    xbmcplugin.endOfDirectory(addon_handle)

    del li

def pair():
    from resources.lib.util.moonlighthelper import MoonlightHelper

    helper = MoonlightHelper(addon, config_helper, logger)
    helper.pair()

    if helper.pair_success:
        address = addon.getSetting('host_addr') or helper.paired_address
        if address:
            persistSetting('host_addr', address)
            config_helper.configure(dump=False)
            config_helper.config['host_addr'] = address
            config_helper.dump()
        _clearApplicationListCache()
        _setPairingStatus(True, addon.getLocalizedString(30321) or 'Paired ✓')
    else:
        syncPairingStatus()

    success = helper.pair_success
    del helper
    return success


def rePair():
    from resources.lib.util.moonlighthelper import MoonlightHelper
    from resources.lib.util.hostapi import Host

    config_helper.configure(dump=False)
    address = addon.getSetting('host_addr') or config_helper.config.get('host_addr')
    if address and not Host(address).has_trusted_pin():
        # Trust was deliberately forgotten. An unpinned response cannot
        # authenticate a previous unpair, so use a fresh native PIN handshake.
        return pair()

    helper = MoonlightHelper(addon, config_helper, logger)
    if not helper.unpair():
        message = helper.last_unpair_error or (
            addon.getLocalizedString(30334)
            or 'Unable to re-pair because the existing host could not be unpaired.')
        xbmcgui.Dialog().ok(addon.getLocalizedString(30000), message)
        syncPairingStatus()
        del helper
        return False

    _clearApplicationListCache()
    _setPairingStatus(False, addon.getLocalizedString(30322) or 'Not paired')
    del helper
    return pair()


def unpairHost():
    from resources.lib.util.moonlighthelper import MoonlightHelper

    config_helper.configure(dump=False)
    address = addon.getSetting('host_addr') or config_helper.config.get('host_addr')
    if not address:
        syncPairingStatus()
        return False

    confirmed = xbmcgui.Dialog().yesno(
        addon.getLocalizedString(30000),
        (addon.getLocalizedString(30331) or 'Unpair from %s?') % address,
        nolabel=xbmc.getLocalizedString(106),
        yeslabel=xbmc.getLocalizedString(107))
    if not confirmed:
        return False

    helper = MoonlightHelper(addon, config_helper, logger)
    success = helper.unpair()
    if success:
        _clearApplicationListCache()
        unverified = getattr(helper, 'last_unpair_unverified', False)
        _setPairingStatus(
            False,
            ('Unpair request completed; status unverified' if unverified
             else (addon.getLocalizedString(30322) or 'Not paired')))
        xbmcgui.Dialog().ok(
            addon.getLocalizedString(30000),
            ('The native unpair request completed, but Sunshine pairing '
             'status could not be authenticated. Verify the paired devices '
             'in Sunshine before assuming this client was removed.'
             if unverified else
             (addon.getLocalizedString(30332) or 'Host unpaired')))
    else:
        xbmcgui.Dialog().ok(
            addon.getLocalizedString(30000),
            helper.last_unpair_error
            or addon.getLocalizedString(30333)
            or 'Unable to unpair from host.')
        syncPairingStatus()
    del helper
    return success


def forgetTrustedServer():
    from resources.lib.util.moonlighthelper import MoonlightHelper

    config_helper.configure(dump=False)
    address = (addon.getSetting('host_addr')
               or config_helper.config.get('host_addr') or '').strip()
    if not address:
        xbmcgui.Dialog().ok('Trusted server certificate',
                            'Select a Sunshine host before forgetting its certificate.')
        return False

    if not xbmcgui.Dialog().yesno(
            'Forget trusted server certificate?',
            ('Remove saved trust for %s?\n\n'
             'Your client keys and Sunshine pairing remain intact. '
             'Proceed only when you expect the server key to have changed. '
             'A new PIN-verified pairing is required afterward.') % address,
            nolabel='Cancel', yeslabel='Forget certificate'):
        return False
    helper = MoonlightHelper(addon, config_helper, logger)
    if not helper.forget_trusted_server(address):
        xbmcgui.Dialog().ok('Trusted server certificate',
                            helper.last_trust_error or 'Unable to forget this host.')
        return False
    _clearApplicationListCache()
    _setPairingStatus(False, 'Host trust cleared; PIN re-pair required')
    xbmcgui.Dialog().ok(
        'Trusted server certificate',
        'Saved trust for %s was cleared. Use Re-Pair Host to authenticate '
        'the new server key. If Sunshine refuses an existing pairing, '
        'remove this client in Sunshine and try again.' % address)
    return True


def pairFromApplications():
    if syncPairingStatus():
        success = rePair()
    else:
        success = pair()
    if success:
        retryApplications()

def quickPlay():
    from resources.lib.util.moonlighthelper import MoonlightHelper

    helper = MoonlightHelper(addon, config_helper, logger)
    detailed = helper.list_games_detailed()
    if detailed:
        items = []
        for _, name, art in detailed:
            li = xbmcgui.ListItem(name)
            if art:
                li.setArt({'icon': art, 'thumb': art, 'poster': art})
            items.append(li)

        chosen = xbmcgui.Dialog().select((addon.getLocalizedString(30250) or 'Applications'), items, useDetails=True)
        if chosen != -1:
            helper.launch_game(detailed[chosen][1])

        xbmc.executebuiltin('Container.Refresh')
        del helper
        return


    games = helper.list_games()
    if games == True:
        pair()
        return
    elif games == False:
        xbmcgui.Dialog().ok(addon.getLocalizedString(30000), addon.getLocalizedString(30018))
        return
    gameId = xbmcgui.Dialog().select((addon.getLocalizedString(30250) or 'Applications'), games)    
    if gameId != -1:
        helper.launch_game(games[gameId])

    xbmc.executebuiltin('Container.Refresh')
    del helper

def resume():
    from resources.lib.util.moonlighthelper import MoonlightHelper
    session = _active_resume_session()
    if not session:
        xbmcgui.Dialog().notification(addon.getLocalizedString(30000),
                                      'No confirmed Resume session for this host.', time=5000)
        return
    confirmed = xbmcgui.Dialog().yesno(
        '', (addon.getLocalizedString(30262) or 'Continue'),
        nolabel=xbmc.getLocalizedString(106),
        yeslabel=xbmc.getLocalizedString(107), autoclose=5000)
    if confirmed and _active_resume_session() == session:
        helper = MoonlightHelper(addon, config_helper, logger)
        helper.launch_game(session[0], resume=True)
        del helper

def quitgame():
    from resources.lib.util.moonlighthelper import MoonlightHelper
    session = _active_resume_session()
    if not session:
        xbmcgui.Dialog().notification(addon.getLocalizedString(30000),
                                      'No confirmed Quit session for this host.', time=5000)
        return
    confirmed = xbmcgui.Dialog().yesno(
        '', (addon.getLocalizedString(30263) or 'Confirm to quit'),
        nolabel=xbmc.getLocalizedString(106),
        yeslabel=xbmc.getLocalizedString(107), autoclose=5000)
    if confirmed and _active_resume_session() == session:
        helper = MoonlightHelper(addon, config_helper, logger)
        # Pass the captured host explicitly; never infer Quit's target from
        # moonlight.conf, which might still point to a previous server.
        if helper.quit_game(session[1]):
            # Do not clear a newer saved session if the settings changed while
            # the native Quit command was running.
            if _active_resume_session() == session:
                addon.setSettingString('last_run', '')
                addon.setSettingString('last_run_host', '')
            xbmcgui.Dialog().ok(addon.getLocalizedString(30000), addon.getLocalizedString(30017))
            xbmc.executebuiltin('Container.Refresh')
        else:
            xbmcgui.Dialog().ok(
                addon.getLocalizedString(30000),
                'Unable to quit the remote session. Resume has been preserved.')

def unpairByKey():
    import shutil
    if os.path.isfile(os.path.expanduser("~") + "/.cache/moonlight/client.p12"):
        check = xbmcgui.Dialog().yesno(
            '', addon.getLocalizedString(30279),
            nolabel=xbmc.getLocalizedString(106),
            yeslabel=xbmc.getLocalizedString(107))
        if check:
            cachePath = os.path.expanduser("~") + '/.cache/moonlight'
            shutil.rmtree(cachePath)
            if not os.path.isdir(cachePath):
                xbmcgui.Dialog().ok('', addon.getLocalizedString(30280))
    else:
        xbmcgui.Dialog().ok('', addon.getLocalizedString(30281))

def _persistSettingXml(ident, value):
    """Synchronize explicitly changed settings while Kodi settings is open."""
    from resources.lib.util.settingsstore import update_setting
    path = translatePath('special://profile/addon_data/%s/settings.xml'
                         % addon.getAddonInfo('id'))
    try:
        update_setting(path, ident, value)
    except (OSError, ValueError, TimeoutError, ElementTree.ParseError) as error:
        logger.error('failed to update %s in %s: %s' % (ident, path, error))


def persistSetting(ident, value):
    dialog_open = xbmc.getCondVisibility('Window.IsActive(addonsettings)')
    if not dialog_open and addon.getSetting(ident) == value:
        return
    addon.setSettingString(ident, value)
    if dialog_open:
        _persistSettingXml(ident, value)


def persistBoolSetting(ident, value):
    dialog_open = xbmc.getCondVisibility('Window.IsActive(addonsettings)')
    addon.setSettingBool(ident, bool(value))
    if dialog_open:
        _persistSettingXml(ident, 'true' if value else 'false')


def selectAudioDevice():
    from resources.lib.util import audiodevices

    devices = audiodevices.list_playback()
    if not devices:
        xbmcgui.Dialog().ok(xbmc.getLocalizedString(257),
                            addon.getLocalizedString(30219))
        return

    busy = addon.getLocalizedString(30217) or 'Busy'
    labels = [audiodevices.describe(d, busy) for d in devices]

    index = xbmcgui.Dialog().select(addon.getLocalizedString(30037), labels)
    if index == -1:
        return

    chosen = devices[index]
    if chosen['busy']:
        confirmed = xbmcgui.Dialog().yesno(
            xbmc.getLocalizedString(14117), addon.getLocalizedString(30218),
            nolabel=xbmc.getLocalizedString(106),
            yeslabel=xbmc.getLocalizedString(107))
        if not confirmed:
            return

    persistSetting('audio_device_parameter', chosen['alsa'])
    persistSetting('audio_device_display', chosen['name'])

    multichannel = audiodevices.multichannel(chosen['alsa'])
    persistSetting('audio_multichannel',
                   'true' if multichannel else 'false')
    if not multichannel and addon.getSetting('surround') != 'false':
        persistSetting('surround', 'false')
        xbmcgui.Dialog().ok(addon.getLocalizedString(30047),
                            addon.getLocalizedString(30266))

def setInputRows(names):
    persistSetting(
        'input_devices_display',
        addon.getLocalizedString(30276 if names else 30272))

def selectInputDevices():
    from resources.lib.util import inputdevices

    all_devices = inputdevices.list_devices()
    invalid_selection = False
    try:
        saved = inputdevices.load_selectors(addon)
    except inputdevices.InvalidInputSelection as error:
        logger.error('Luna saved controller selection is invalid: %s' % error)
        xbmcgui.Dialog().ok(
            'Controller selection needs repair',
            'The saved controller selection is invalid. Reselect your '
            'controllers here (or clear them) to repair it.')
        saved = []
        invalid_selection = True

    options = inputdevices.picker_options(all_devices, saved)
    if not options:
        if invalid_selection:
            if xbmcgui.Dialog().yesno(
                    'Clear invalid controller selection?',
                    'No controllers are currently available. Clear the '
                    'invalid saved selection to restore automatic input?'):
                persistSetting('input_device_selectors_v2',
                               inputdevices.serialize_selectors([]))
                persistSetting('input_devices', '')
                setInputRows([])
        else:
            xbmcgui.Dialog().ok(addon.getLocalizedString(30269),
                                addon.getLocalizedString(30273))
        return

    kinds = {'keyboard': addon.getLocalizedString(30292),
             'mouse':    addon.getLocalizedString(30293),
             'gamepad':  addon.getLocalizedString(30294),
             'other':    addon.getLocalizedString(30295)}
    labels = []
    for option in options:
        device = option['device']
        identity = option['selector']
        if option.get('group'):
            label = identity['name'] + ' [all same-name devices]'
            if option['offline']:
                label += ' [offline]'
        elif option['offline']:
            label = identity['name'] + ' [offline]'
        else:
            label = inputdevices.describe(device, kinds,
                                          addon.getLocalizedString(30296))
        same_name_count = sum(d['name'] == identity['name'] for d in all_devices)
        if not option.get('group') and (
                identity['kind'] == 'N' or same_name_count > 1 or
                option['offline']):
            if identity['kind'] == 'N':
                label += ' [all same-name devices]'
            else:
                detail = identity.get('phys') or identity['value']
                label += ' [%s: ...%s]' % (
                    'port' if identity['kind'] == 'P' else 'device',
                    detail[-12:])
        labels.append(label)

    preselect = [i for i, option in enumerate(options) if option['selected']]
    chosen = xbmcgui.Dialog().multiselect(addon.getLocalizedString(30269),
                                          labels, preselect=preselect)
    if chosen is None:
        return

    records = [options[i]['selector'] for i in chosen]
    # A name-only selector also admits controllers connected in the future.
    # Require explicit acknowledgment when it is newly selected because the
    # device has no reliable individual identity.
    new_groups = sorted({entry['name'] for entry in records
                         if entry['kind'] == 'N' and entry not in saved})
    if new_groups and not xbmcgui.Dialog().yesno(
            'Select all matching controllers?',
            'These devices cannot be uniquely identified. Selecting '
            'a name group also allows any future controllers with the '
            'same name: %s. Continue?' % ', '.join(new_groups)):
        return
    names = [entry['name'] for entry in records]
    persistSetting('input_device_selectors_v2',
                   inputdevices.serialize_selectors(records))
    persistSetting('input_devices', '|'.join(names))
    setInputRows(names)

def selectLaunchscripts():
    launchscripts = os.listdir(getAddonPath('/resources/launchscripts/'))
    ret = xbmcgui.Dialog().select(
        addon.getLocalizedString(30282), launchscripts)
    if ret != -1:
        persistSetting('launchscript_conf', launchscripts[ret])

ensurePermissions()

if config_helper.binary_path is None:
    xbmcgui.Dialog().ok(addon.getLocalizedString(30283),
                        addon.getLocalizedString(30284))
    exit()

stale = addon.getSetting('saved_streamsilence')
if stale:
    try:
        import json
        xbmc.executeJSONRPC(json.dumps({
            'jsonrpc': '2.0', 'id': 1, 'method': 'Settings.SetSettingValue',
            'params': {'setting': 'audiooutput.streamsilence', 'value': int(stale)}}))
    except Exception:
        pass
    addon.setSettingString('saved_streamsilence', '')

chosen = addon.getSetting('launchscript_conf')
if not chosen or not os.path.isdir(getAddonPath('/resources/launchscripts/' + chosen)):
    addon.setSettingString('launchscript_conf', 'coreelec')

try:
    from resources.lib.util import audiodevices as _audiodevices
    _names = [n for n in addon.getSetting('input_devices').split('|') if n]
    setInputRows(_names)

    _device = addon.getSetting('audio_device_parameter')
    if _device:
        persistSetting('audio_device_display', _audiodevices.pretty(_device))
        persistSetting(
            'audio_multichannel',
            'true' if _audiodevices.multichannel(_device) else 'false')
except Exception:
    pass

if not addon.getSetting('host_addr') and config_helper.config.get('host_addr'):
    addon.setSettingString('host_addr', config_helper.config['host_addr'])

version = xbmcaddon.Addon().getAddonInfo('version')
if version != addon.getSetting('addon_version'):
    addon.setSetting('addon_version', version)

try:
    handled_directory = False

    if 'action' in args:
        action = args['action'][0]
        if action == 'quick_play':
            quickPlay()
        elif action == 'launch':
            launchByName()
        elif action == 'refresh':
            refreshGames()
            xbmcplugin.endOfDirectory(addon_handle, succeeded=False)
            handled_directory = True
        elif action == 'apps':
            showApps(args.get('folder', [None])[0])
            handled_directory = True
        elif action == 'newfolder':
            newFolder()
            xbmcplugin.endOfDirectory(addon_handle, succeeded=False)
            xbmc.executebuiltin('Container.Refresh')
            handled_directory = True
        elif action == 'assign':
            assignApp()
        elif action == 'renfolder':
            renameFolder()
        elif action == 'delfolder':
            deleteFolder()
        elif action == 'resume':
            resume()
        elif action == 'quit':
            quitgame()
        elif action == 'pair_host':
            pair()
        elif action == 'repair_host':
            rePair()
        elif action == 'unpair_host':
            unpairHost()
        elif action == 'forget_trusted_server':
            forgetTrustedServer()
        elif action == 'retry_apps':
            retryApplications()
        elif action == 'pair_from_apps':
            pairFromApplications()
        elif action == 'select_audio_device':
            selectAudioDevice()
        elif action == 'select_input_devices':
            selectInputDevices()
        elif action == 'select_launchscripts':
            selectLaunchscripts()
        elif action == 'unpair_by_key':
            unpairByKey()
        elif action == 'settings':
            settings()

    if addon_handle != -1 and not handled_directory:
        main()

except InvalidInputSelection as error:
    logger.error('Luna saved controller selection prevented action: %s' % error)
    xbmcgui.Dialog().ok(
        'Controller selection needs repair',
        'The saved controller identities are invalid. No controller selection '
        'was applied. Open Luna Settings, select Input Devices, and reselect '
        'or clear controllers before retrying.')

del addon
del logger
del config_helper