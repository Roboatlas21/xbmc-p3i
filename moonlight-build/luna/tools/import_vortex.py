#!/usr/bin/env python3
"""Import source/launcher assets from Vortex's checksum-verified Luna NO ZIP.

Do not import the compiled AArch64 Moonlight binaries.
"""
from pathlib import Path
import hashlib
import re
import sys
import zipfile

EXPECTED = '74c93039401b146b87fb67b62f95f1054834d48fba0bb5d88c64c639b78d0837'
ALLOWED = {'.py', '.xml', '.po', '.sh', '.md', '.txt', '.png', '.jpg', '.mp4'}


def replace_once(data, old, new, label):
    if data.count(old) != 1:
        raise RuntimeError(f'{label}: expected one match; found {data.count(old)}')
    return data.replace(old, new, 1)


def import_zip(source, dest):
    if hashlib.sha256(source.read_bytes()).hexdigest() != EXPECTED:
        raise ValueError('ZIP SHA256 mismatch; refusing to import unverified release')
    with zipfile.ZipFile(source) as archive:
        for member in archive.namelist():
            if not member.startswith('script.luna/') or member.endswith('/'):
                continue
            rel = member[len('script.luna/'):]
            path = Path(rel)
            if (path.is_absolute() or '..' in path.parts or path.suffix not in ALLOWED
                    or path.name == 'LICENSE.txt' or path.parts[0] in ('bin', 'lib')):
                continue
            target = dest / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(member))

    config = dest / 'resources/lib/util/confighelper.py'
    code = config.read_text(encoding='utf-8')
    code = replace_once(code, 'import os\nimport xbmcaddon',
                        'import os\nimport struct\nimport xbmcaddon', 'config imports')
    code, count = re.subn(r'    @staticmethod\n    def _find_binary\(internal_path\):[\s\S]*$', '''    @staticmethod
    def _find_binary(internal_path):
        # Use Kodi's userspace bitness, not the kernel's architecture.
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
            if not os.path.isfile(candidate) or not os.access(candidate, os.X_OK):
                continue
            try:
                with open(candidate, 'rb') as executable:
                    header = executable.read(5)
                if len(header) != 5 or header[:4] != bytes((127,)) + b'ELF' or header[4] != elf_class:
                    continue
            except OSError:
                continue
            return os.path.dirname(candidate) + '/'
        return None
''', code, count=1)
    if count != 1:
        raise RuntimeError('Missing Vortex binary selector')
    config.write_text(code, encoding='utf-8')

    helper = dest / 'resources/lib/util/moonlighthelper.py'
    code = helper.read_text(encoding='utf-8')
    start = code.index('def env_with_libs(')
    end = code.index('class MoonlightHelper:', start)
    code = code[:start] + '''def env_with_libs(binary_path, addon_path=None):
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


''' + code[end:]
    code = code.replace('env_with_libs(binary_path)',
                        'env_with_libs(binary_path, self.config_helper.addon_path)')
    old = "launch_cmd = subprocess.Popen([scripts_path + 'launch.sh', game_id], cwd=binary_path, env=env_with_libs(binary_path, self.config_helper.addon_path), start_new_session=True)"
    new = "env = moonlight_environment(self.config_helper)\n            env['MOONLIGHT_BINARY'] = binary_path + 'moonlight'\n            launch_cmd = subprocess.Popen([scripts_path + 'launch.sh', game_id], cwd=binary_path, env=env, start_new_session=True)"
    code = replace_once(code, old, new, 'launch environment')
    helper.write_text(code, encoding='utf-8')

    addon = dest / 'addon.py'
    code = addon.read_text(encoding='utf-8')
    code = replace_once(code, "targets = [getAddonPath('/bin/moonlight'),",
                        "targets = [getAddonPath('/bin/coreelec-ng/moonlight'),\n               getAddonPath('/bin/coreelec-no/moonlight'),\n               getAddonPath('/bin/moonlight'),",
                        'executable permissions')
    addon.write_text(code, encoding='utf-8')

    launcher = dest / 'resources/launchscripts/coreelec/launch.sh'
    launcher.write_text('''#!/usr/bin/sh
set -u
BIN="${MOONLIGHT_BINARY:-moonlight}"
LOG="${LUNA_LOG_DIR:-/storage/.kodi/userdata/addon_data/script.luna}/logfile"
mkdir -p "$(dirname "$LOG")" || exit 1
if ! command -v "$BIN" >/dev/null 2>&1; then
  echo "Moonlight executable not found: $BIN" >&2
  exit 127
fi
resume_kodi() { killall -CONT kodi.bin 2>/dev/null || true; }
trap resume_kodi EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
if [ -f "$LOG" ]; then mv -f "$LOG" "$LOG.old" 2>/dev/null || true; fi
killall -STOP kodi.bin 2>/dev/null || true
"$BIN" stream -app "$@" >> "$LOG" 2>&1
''', encoding='utf-8')
    launcher.chmod(0o755)
    for path in (dest / 'resources/launchscripts/coreelec').glob('*.sh'):
        path.chmod(0o755)
    print('Imported verified Vortex Luna source and NG/NO adaptations')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('Usage: import_vortex.py VORTEX_ZIP DESTINATION')
    import_zip(Path(sys.argv[1]), Path(sys.argv[2]))
