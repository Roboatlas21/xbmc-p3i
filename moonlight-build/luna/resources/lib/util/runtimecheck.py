"""Safe Luna NG loader preflight, run before Kodi or its audio is suspended.

Checks installed bytes against the manifest, then invokes 'moonlight help' to
verify the executable loader and its direct/transitive DT_NEEDED libraries.
Does not start any stream or load the Amlogic decoder video module.
"""
import hashlib
import json
import os
import re
import struct
import subprocess

class NativeRuntimeError(RuntimeError):
    pass

REQUIRED = {
    "bin/coreelec-ng/moonlight",
    "lib/coreelec-ng/libmoonlight-aml.so",
    "lib/coreelec-ng/libgamestream.so.4",
    "lib/coreelec-ng/libmoonlight-common.so.4",
    "lib/coreelec-ng/libcec.so.6",
}
ASSETS = {"bin/coreelec-ng/gamecontrollerdb.txt"}

def check_ng_runtime(addon_path, executable, env=None, check_loader=True):
    root = os.path.abspath(addon_path)
    expected = os.path.join(root, "bin", "coreelec-ng", "moonlight")
    # Do not impose the NG package contract on a system-installed binary
    # or the existing CoreELEC NO/AArch64 release.
    if os.path.abspath(executable) != expected:
        return False
    location = os.path.join(root, "runtime-manifest.json")
    try:
        if os.path.getsize(location) > 128 * 1024:
            raise NativeRuntimeError("Native manifest is oversized")
        with open(location, "r", encoding="utf-8") as file:
            manifest = json.load(file)
    except (OSError, ValueError) as exc:
        raise NativeRuntimeError("Missing or malformed NG runtime manifest") from exc
    if not isinstance(manifest, dict) or (
        manifest.get("schema") != "luna-ng-runtime-v1" or
        manifest.get("target") != "Amlogic-ce/Amlogic-ng/arm"
    ):
        raise NativeRuntimeError("Wrong CoreELEC NG runtime manifest")
    objects = manifest.get("objects")
    assets = manifest.get("assets")
    if (not isinstance(objects, dict) or set(objects) != REQUIRED or
            not isinstance(assets, dict) or set(assets) != ASSETS):
        raise NativeRuntimeError("NG native artifact files do not match manifest")
    for mapping in (objects, assets):
        for name, entry in mapping.items():
            if not isinstance(entry, dict) or not re.fullmatch(r"[a-f0-9]{64}", str(entry.get("sha256"))):
                raise NativeRuntimeError("Invalid native hash entry: " + name)
            path = os.path.join(root, *name.split("/"))
            try:
                if os.path.islink(path) or not os.path.isfile(path):
                    raise NativeRuntimeError("Missing native file: " + name)
                checksum = hashlib.sha256()
                size = 0
                with open(path, "rb") as file:
                    while True:
                        chunk = file.read(128 * 1024)
                        if not chunk:
                            break
                        size += len(chunk)
                        checksum.update(chunk)
                if size != entry.get("size") or checksum.hexdigest() != entry["sha256"]:
                    raise NativeRuntimeError("Native file checksum mismatch: " + name)
                if name in REQUIRED:
                    with open(path, "rb") as file:
                        header = file.read(52)
                    if (len(header) < 52 or header[:6] != b"\x7fELF\x01\x01" or
                            struct.unpack_from("<H", header, 18)[0] != 40):
                        raise NativeRuntimeError("Not a CoreELEC NG ARM32 ELF: " + name)
            except OSError as exc:
                raise NativeRuntimeError("Cannot read native file: " + name) from exc
    if check_loader:
        interp = objects["bin/coreelec-ng/moonlight"].get("interpreter")
        if not isinstance(interp, str) or not interp.startswith("/"):
            raise NativeRuntimeError("Invalid dynamic loader path")
        if not os.path.isfile(interp):
            raise NativeRuntimeError("Required ARM32 dynamic loader not installed: " + interp)
    try:
        completed = subprocess.run(
            [expected, "help"], cwd=os.path.dirname(expected), env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            errors="replace", timeout=6, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise NativeRuntimeError("Moonlight loader smoke test failed") from exc
    if completed.returncode != 0 or "Moonlight Embedded" not in completed.stdout:
        # Omit full stdout; it may include host or local environment details.
        raise NativeRuntimeError(
            "Moonlight help failed (exit %d); verify external ARM32 libraries" %
            completed.returncode)
    return True
