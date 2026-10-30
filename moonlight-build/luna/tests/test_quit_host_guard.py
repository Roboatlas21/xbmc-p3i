"""Exercise the real addon Resume/Quit guards without Kodi's import side effects."""
import ast
import hashlib
from pathlib import Path
import types
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TestQuitHostGuard(unittest.TestCase):
    def test_ui_captures_verified_host_and_passes_it_to_native_quit(self):
        source = ast.parse((ROOT / 'addon.py').read_text(encoding='utf-8'))
        names = {'_active_resume_session', '_active_resume_app', 'quitgame'}
        selected = [node for node in source.body if isinstance(node, ast.FunctionDef)
                    and node.name in names]
        class Library:
            @staticmethod
            def host_identity(host):
                return hashlib.sha256(host.strip().lower().encode()).hexdigest()[:20]
        module = types.ModuleType('resources.lib.util.library')
        module.Library = Library
        module2 = types.ModuleType('resources.lib.util.moonlighthelper')
        called = []
        class Helper:
            def __init__(self, *args):
                pass
            def quit_game(self, host):
                called.append(host)
                return True
        module2.MoonlightHelper = Helper
        original_lib = sys.modules.get(module.__name__)
        original_helper = sys.modules.get(module2.__name__)
        sys.modules[module.__name__] = module
        sys.modules[module2.__name__] = module2
        try:
            class Addon:
                settings = {'host_addr': 'host-A', 'last_run': 'Desktop',
                            'last_run_host': Library.host_identity('host-A')}
                def getSetting(self, key):
                    return self.settings.get(key, '')
                def setSettingString(self, key, value):
                    self.settings[key] = value
                def getLocalizedString(self, key):
                    return ''
            class Dialog:
                def yesno(self, *args, **kw):
                    return True
                def notification(self, *args, **kw):
                    pass
                def ok(self, *args, **kw):
                    pass
            addon = Addon()
            ns = {'addon': addon, 'config_helper': types.SimpleNamespace(
                      config={'host_addr': 'host-B'}),
                  'logger': None,
                  'xbmcgui': types.SimpleNamespace(Dialog=Dialog),
                  'xbmc': types.SimpleNamespace(getLocalizedString=lambda _: '',
                         executebuiltin=lambda _: None)}
            code = compile(ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[])),
                           'addon.py', 'exec')
            exec(code, ns)
            ns['quitgame']()
            self.assertEqual(called, ['host-A'])
            self.assertEqual(addon.getSetting('last_run'), '')
            addon.settings['host_addr'] = 'host-B'
            addon.settings['last_run'] = 'Desktop'
            addon.settings['last_run_host'] = Library.host_identity('host-A')
            ns['quitgame']()
            self.assertEqual(called, ['host-A'])
            self.assertEqual(addon.getSetting('last_run'), 'Desktop')
        finally:
            if original_lib is None:
                sys.modules.pop(module.__name__, None)
            else:
                sys.modules[module.__name__] = original_lib
            if original_helper is None:
                sys.modules.pop(module2.__name__, None)
            else:
                sys.modules[module2.__name__] = original_helper


if __name__ == '__main__':
    unittest.main()
