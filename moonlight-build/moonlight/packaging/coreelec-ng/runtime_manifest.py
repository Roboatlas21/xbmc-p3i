#!/usr/bin/env python3
"""Build a verifiable ARM32 runtime manifest without executing target binaries."""
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

SCHEMA = "luna-ng-runtime-v1"
ASSETS = ("bin/coreelec-ng/gamecontrollerdb.txt",)
OBJECTS = (
    "bin/coreelec-ng/moonlight",
    "lib/coreelec-ng/libmoonlight-aml.so",
    "lib/coreelec-ng/libgamestream.so.4",
    "lib/coreelec-ng/libmoonlight-common.so.4",
    "lib/coreelec-ng/libcec.so.6",
)
SONAMES = {
    "libmoonlight-aml.so": "libmoonlight-aml.so",
    "libgamestream.so.4": "libgamestream.so.4",
    "libmoonlight-common.so.4": "libmoonlight-common.so.4",
    "libcec.so.6": "libcec.so.6",
}
NEEDED = re.compile(r"\(NEEDED\).*Shared library: \[([^\]]+)\]")
SONAME = re.compile(r"\(SONAME\).*Library soname: \[([^\]]+)\]")
INTERP = re.compile(r"Requesting program interpreter:\s*([^\]]+)\]")

def elf_properties(data, name):
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("Not an ELF32 little-endian object: " + name)
    etype, machine = struct.unpack_from("<HH", data, 16)
    if machine != 40 or etype not in (2, 3):
        raise ValueError("Wrong ARM machine/ELF type for: " + name)
    return {"class": "ELF32", "machine": "ARM", "e_flags": struct.unpack_from("<I", data, 36)[0]}

def elf_dynamic(output):
    return sorted(set(NEEDED.findall(output))), (SONAME.search(output).group(1) if SONAME.search(output) else None)

def program_interpreter(output):
    match = INTERP.search(output)
    return match.group(1) if match else None

def readelf(flag, path):
    result = subprocess.run(["readelf", flag, str(path)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, encoding="utf-8",
                            errors="replace", timeout=20, check=True)
    return result.stdout

def build_manifest(root, source_sha):
    root = Path(root)
    if not (re.fullmatch(r"[a-f0-9]{40}", source_sha) or source_sha == "unknown"):
        raise ValueError("Invalid source revision identifier")
    objects = {}
    private_sonames = set(SONAMES)
    for name in OBJECTS:
        p = root / name
        if not p.is_file() or p.is_symlink():
            raise ValueError("Missing expected regular native ELF: " + name)
        data = p.read_bytes()
        abi = elf_properties(data, name)
        needed, soname = elf_dynamic(readelf("-d", p))
        if name.startswith("lib/") and soname != SONAMES[p.name]:
            raise ValueError("Unexpected private library SONAME for %s: %s" % (name, soname))
        if name.startswith("bin/") and soname is not None:
            raise ValueError("Executable unexpectedly has a shared-library SONAME")
        interp = program_interpreter(readelf("-l", p)) if name.startswith("bin/") else None
        if name.startswith("bin/") and (not interp or not interp.startswith("/")):
            raise ValueError("Cannot determine native ARM32 dynamic loader")
        dynamic = readelf("-d", p)
        if "RPATH" in dynamic or "RUNPATH" in dynamic:
            raise ValueError("Unexpected embedded loader search paths in " + name)
        objects[name] = {
            **abi, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "needed": needed, "soname": soname, "interpreter": interp,
        }
    if "libgamestream.so.4" not in objects[OBJECTS[0]]["needed"]:
        raise ValueError("Moonlight executable has no expected libgamestream dependency")
    if "libmoonlight-common.so.4" not in objects[OBJECTS[2]]["needed"]:
        raise ValueError("libgamestream is not linked to the bundled common library")
    # The Amlogic module is loaded via dlopen() and may need only libamcodec.
    # It does not necessarily retain a direct DT_NEEDED on libgamestream.
    assets = {}
    for name in ASSETS:
        asset = root / name
        if not asset.is_file() or asset.is_symlink():
            raise ValueError("Missing required native input mapping: " + name)
        content = asset.read_bytes()
        if not 100 <= len(content) <= 2 * 1024 * 1024:
            raise ValueError("Unexpected input mapping size: " + name)
        assets[name] = {"size": len(content),
                        "sha256": hashlib.sha256(content).hexdigest()}
    external = sorted({lib for v in objects.values() for lib in v["needed"]
                       if lib not in private_sonames})
    return {
        "schema": SCHEMA, "source_git_sha": source_sha,
        "target": "Amlogic-ce/Amlogic-ng/arm",
        "objects": objects, "assets": assets, "external_needed": external,
        "external_library_check": "not verified on target device",
    }

def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: runtime_manifest.py STAGE_DIR SOURCE_GIT_SHA")
    root = Path(sys.argv[1])
    manifest = build_manifest(root, sys.argv[2])
    (root / "runtime-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("Validated ARM32 runtime manifest (%d external SONAMEs): %s" %
          (len(manifest["external_needed"]),
           ", ".join(manifest["external_needed"]) or "none"))

if __name__ == "__main__":
    main()
