"""Optional VFM bundle validation. No Kodi imports and no device mutations."""
import hashlib
import fcntl
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess


class VfmRuntimeError(RuntimeError):
    pass


def verify_bundle(directory, kernel_release=None):
    root = Path(directory)
    try:
        manifest_path = root / 'manifest.json'
        if manifest_path.is_symlink() or manifest_path.stat().st_size > 65536:
            raise VfmRuntimeError('Invalid experimental pipeline manifest')
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        if not isinstance(manifest, dict) or manifest.get('schema') != 'luna-ng-vfm-v1':
            raise VfmRuntimeError('Unsupported experimental pipeline bundle')
        if manifest.get('bridge_version') != '4':
            raise VfmRuntimeError('This VFM bundle needs bridge version 4. Install a complete '
                                  'updated add-on; rebooting cannot update the bundled driver.')
        release = manifest.get('kernel_release', '')
        if not isinstance(release, str) or not re.fullmatch(r'[A-Za-z0-9._+-]+', release):
            raise VfmRuntimeError('Invalid kernel release in experimental bundle')
        if kernel_release is not None and release != kernel_release:
            raise VfmRuntimeError('Experimental drivers are built for %s, running %s' %
                                  (release, kernel_release))
        objects = manifest.get('objects')
        order = manifest.get('module_order')
        if not isinstance(objects, dict) or not objects or len(objects) > 64:
            raise VfmRuntimeError('Invalid experimental file list')
        if (not isinstance(order, list) or not order or
                any(not isinstance(n, str) or not re.fullmatch(r'[A-Za-z0-9_]+\.ko', n)
                    for n in order) or len(order) != len(set(order)) or
                order[-1] != 'moonlight_vfm.ko'):
            raise VfmRuntimeError('Invalid experimental module load order')
        modules = {'modules/%s/%s' % (release, n) for n in order}
        libraries = set(objects) - modules
        if ('lib/libmoonlight-aml-vfm.so' not in libraries or
                not any(re.fullmatch(r'lib/libavcodec\.so\.\d+', n) for n in libraries) or
                not any(re.fullmatch(r'lib/libavutil\.so\.\d+', n) for n in libraries) or
                any(not re.fullmatch(
                    r'lib/(libmoonlight-aml-vfm\.so|libavcodec\.so\.\d+|libavutil\.so\.\d+|libdrm\.so\.2)', n)
                    for n in libraries) or not modules <= set(objects)):
            raise VfmRuntimeError('Missing or unexpected experimental libraries/modules')
        for name, entry in objects.items():
            rel = PurePosixPath(name)
            if rel.is_absolute() or '..' in rel.parts or not isinstance(entry, dict):
                raise VfmRuntimeError('Unsafe experimental file path')
            path = root.joinpath(*rel.parts)
            if any(p.is_symlink() for p in [root, path, *path.parents] if p != p.parent):
                raise VfmRuntimeError('Experimental bundle contains a symlink')
            if not path.is_file() or not 20 <= path.stat().st_size <= 64 * 1024 * 1024:
                raise VfmRuntimeError('Missing or oversized experimental file: ' + name)
            data = path.read_bytes()
            if (entry.get('size') != len(data) or
                    entry.get('sha256') != hashlib.sha256(data).hexdigest()):
                raise VfmRuntimeError('Experimental file checksum mismatch: ' + name)
            elf_class, machine, kind = (2, 183, 1) if name in modules else (1, 40, 3)
            if (data[:6] != b'\x7fELF' + bytes([elf_class, 1]) or
                    struct.unpack_from('<HH', data, 16) != (kind, machine)):
                raise VfmRuntimeError('Wrong ELF architecture/type: ' + name)
        return manifest
    except (OSError, ValueError, TypeError, struct.error) as error:
        raise VfmRuntimeError('Incomplete experimental pipeline bundle: %s' % error) from error


def check_runtime(addon_path, executable, env):
    """Verify optional files and FFmpeg capabilities before stopping Kodi."""
    root = Path(addon_path) / 'vfm'
    verify_bundle(root, os.uname().release)
    check_loaded_bridge()
    try:
        result = subprocess.run(
            [executable, 'check-vfm'], env=env, cwd=str(Path(executable).parent),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            timeout=8, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VfmRuntimeError('Experimental pipeline library check failed') from error
    if result.returncode or 'AML-VFM userspace dependencies ready' not in result.stdout:
        raise VfmRuntimeError('This Moonlight/FFmpeg build lacks the experimental VFM pipeline. '
                              'Select the standard Amlogic pipeline.')


def check_loaded_bridge():
    bridge = Path('/sys/module/moonlight_vfm')
    if bridge.exists():
        version = bridge / 'version'
        try:
            loaded_version = version.read_text().strip()
        except OSError as error:
            raise VfmRuntimeError('Cannot read the loaded VFM bridge version') from error
        if loaded_version != '4':
            raise VfmRuntimeError('An older VFM bridge is still loaded. Reboot CoreELEC once '
                                  'after this add-on update, then retry VFM.')


def load_modules(directory):
    """Called only by the independent launcher, after Kodi has stopped."""
    root = Path(directory)
    manifest = verify_bundle(root, os.uname().release)
    check_loaded_bridge()
    for name in manifest['module_order']:
        if (Path('/sys/module') / name[:-3]).exists():
            continue
        module = root / 'modules' / manifest['kernel_release'] / name
        info = subprocess.run(['modinfo', '-F', 'vermagic', str(module)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, timeout=5, check=True)
        if info.stdout.split()[:1] != [os.uname().release]:
            raise VfmRuntimeError('Kernel mismatch for ' + name)
        if name == 'moonlight_vfm.ko':
            info = subprocess.run(['modinfo', '-F', 'version', str(module)],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, timeout=5, check=True)
            if info.stdout.strip() != '4':
                raise VfmRuntimeError('Bundled VFM bridge is not version 4')
        command = ['insmod', str(module)]
        if name == 'amvdec_ports.ko':
            command.append('multiplanar=1')
        subprocess.run(command, timeout=15, check=True)
    if not Path('/dev/moonlight-vfm').exists():
        raise VfmRuntimeError('Experimental VFM bridge device did not appear')
    check_loaded_bridge()
    check_decoder_device()
    # Do not rmmod: the session closes its provider and buffers; modules remain.


def check_decoder_device(sys_root='/sys', dev_root='/dev'):
    """Check the actual stock decoder after module activation, with Kodi stopped."""
    mode = Path(sys_root) / 'module/amvdec_ports/parameters/multiplanar'
    if mode.read_text().strip() not in ('1', 'Y'):
        mode.write_text('1\n')
    if mode.read_text().strip() not in ('1', 'Y'):
        raise VfmRuntimeError('Cannot enable multiplanar output on the stock decoder')

    devices = Path(sys_root) / 'class/video4linux'
    for entry in sorted(devices.glob('video*')):
        if (entry / 'name').read_text().strip() != 'aml-vcodec-dec':
            continue
        path = Path(dev_root) / entry.name
        fd = os.open(path, os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
        try:
            # These two V4L2 structs contain only fixed-width fields; their
            # ioctl numbers/layouts are identical for ARM32 and ARM64.
            cap = bytearray(104)
            fcntl.ioctl(fd, 0x80685600, cap)  # VIDIOC_QUERYCAP
            caps, device_caps = struct.unpack_from('=II', cap, 84)
            if caps & 0x80000000:  # V4L2_CAP_DEVICE_CAPS
                caps = device_caps
            if caps & 0x04004000 != 0x04004000:  # STREAMING | VIDEO_M2M_MPLANE
                raise VfmRuntimeError('%s lacks streaming/multiplanar decoder capabilities' % path)
            for queue, required in ((10, {b'H264', b'HEVC'}), (9, {b'NV12'})):
                formats = set()
                for index in range(64):
                    desc = bytearray(64)
                    struct.pack_into('=II', desc, 0, index, queue)
                    try:
                        fcntl.ioctl(fd, 0xc0405602, desc)  # VIDIOC_ENUM_FMT
                    except OSError as error:
                        if error.errno != 22:
                            raise
                        break
                    formats.add(bytes(desc[44:48]))
                if not required <= formats:
                    raise VfmRuntimeError('%s lacks required %s formats (reported %s)' %
                                          (path, 'input' if queue == 10 else 'capture',
                                           sorted(f.decode('ascii', 'replace') for f in formats)))
            print('AML-VFM: stock decoder ready at %s (H.264/HEVC, NV12)' % path)
            return
        finally:
            os.close(fd)
    raise VfmRuntimeError('Stock V4L2 decoder device did not appear after runtime activation')


if __name__ == '__main__':
    import sys
    try:
        if len(sys.argv) != 3 or sys.argv[1] != 'load':
            raise VfmRuntimeError('Usage: vfmruntime.py load ADDON_VFM_DIRECTORY')
        load_modules(sys.argv[2])
    except (VfmRuntimeError, OSError, subprocess.SubprocessError) as error:
        print('AML-VFM: %s' % error, file=sys.stderr)
        sys.exit(1)
