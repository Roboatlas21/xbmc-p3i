#!/usr/bin/env python3
"""Create an installable Luna NG Kodi ZIP from a verified Moonlight ARM32 artifact.

Usage:
  python3 packaging/package_ng.py moonlight-coreelec21-ng-arm32.tar.gz --output luna-ng.zip

This tool never fetches another private repository or installs binaries on a TV.
"""
import argparse
import importlib.util
import hashlib
import io
import json
import posixpath
import re
import stat
import struct
import tarfile
import zipfile
from pathlib import Path
from xml.etree import ElementTree

NATIVE_FILES = frozenset({
    "bin/coreelec-ng/moonlight",
    "lib/coreelec-ng/libmoonlight-aml.so",
    "lib/coreelec-ng/libgamestream.so.4",
    "lib/coreelec-ng/libmoonlight-common.so.4",
    "lib/coreelec-ng/libcec.so.6",
})
MANIFEST = "runtime-manifest.json"
ASSETS = frozenset({"bin/coreelec-ng/gamecontrollerdb.txt"})
OPTIONAL_FILES = frozenset({"build-info.txt", MANIFEST})
SCHEMA = "luna-ng-runtime-v1"
PRIVATE_SONAMES = frozenset({"libmoonlight-aml.so", "libgamestream.so.4",
                            "libmoonlight-common.so.4", "libcec.so.6"})
EXPECTED_LIB_SONAMES = {
    "lib/coreelec-ng/libmoonlight-aml.so": "libmoonlight-aml.so",
    "lib/coreelec-ng/libgamestream.so.4": "libgamestream.so.4",
    "lib/coreelec-ng/libmoonlight-common.so.4": "libmoonlight-common.so.4",
    "lib/coreelec-ng/libcec.so.6": "libcec.so.6",
}

# Only the trap-protected CoreELEC launcher is included in the NG test ZIP.
# Legacy USB presets globally reset attached USB devices; retain in source,
# but do not ship them for CoreELEC 21.
NG_LAUNCHER = "coreelec"
# Exclude only top-level project/dev folders. In particular, `resources/lib`
# contains Luna's REQUIRED Python modules; rejecting any path component named
# "lib" mistakenly produced a ZIP without those modules.
EXCLUDED_ROOTS = frozenset({
    ".git", ".github", "tests", "docs", "packaging", "tools", "bin", "lib", "vfm",
})
EXCLUDED_CACHE_DIRS = frozenset({"__pycache__", ".pytest_cache"})


def assert_arm32(data, name):
    # ARM ELF32, little-endian, e_machine=40. Do not trust only the extension.
    if (len(data) < 20 or data[:4] != bytes((127,)) + b"ELF"
            or data[4:6] != b"\x01\x01"
            or struct.unpack_from("<H", data, 18)[0] != 40):
        raise ValueError("Not a 32-bit little-endian ARM ELF: " + name)


def verify_runtime_manifest(found):
    if MANIFEST not in found:
        raise ValueError("Missing ARM32 runtime manifest (rebuild from updated collector)")
    if len(found[MANIFEST]) > 128 * 1024:
        raise ValueError("Oversized native runtime manifest")
    try:
        manifest = json.loads(found[MANIFEST].decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as error:
        raise ValueError("Malformed ARM32 runtime manifest") from error
    if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA:
        raise ValueError("Unsupported ARM32 runtime manifest schema")
    if manifest.get("target") != "Amlogic-ce/Amlogic-ng/arm":
        raise ValueError("Native manifest target does not match CoreELEC 21 NG")
    source = manifest.get("source_git_sha")
    if not isinstance(source, str) or not (
        re.fullmatch(r"[a-f0-9]{40}", source) or source == "unknown"
    ):
        raise ValueError("Invalid native source revision in manifest")
    assets = manifest.get("assets")
    if not isinstance(assets, dict) or set(assets) != ASSETS:
        raise ValueError("Native asset list differs from manifest")
    for name in ASSETS:
        if name not in found or not 100 <= len(found[name]) <= 2 * 1024 * 1024:
            raise ValueError("Missing or invalid native input mapping: " + name)
        expected = assets[name]
        if not isinstance(expected, dict) or (
            expected.get("size") != len(found[name]) or
            expected.get("sha256") != hashlib.sha256(found[name]).hexdigest()
        ):
            raise ValueError("Native input mapping digest mismatch: " + name)
    objects = manifest.get("objects")
    if not isinstance(objects, dict) or set(objects) != NATIVE_FILES:
        raise ValueError("Native artifact objects differ from manifest")
    external = set()
    for name in NATIVE_FILES:
        obj = objects[name]
        if not isinstance(obj, dict) or (
            obj.get("class") != "ELF32" or obj.get("machine") != "ARM"
        ):
            raise ValueError("Wrong ABI in native manifest: " + name)
        if obj.get("size") != len(found[name]) or (
            obj.get("sha256") != hashlib.sha256(found[name]).hexdigest()
        ):
            raise ValueError("Native object digest mismatch: " + name)
        if obj.get("soname") != EXPECTED_LIB_SONAMES.get(name):
            raise ValueError("Native library SONAME mismatch: " + name)
        if name.startswith("bin/"):
            interp = obj.get("interpreter")
            if not isinstance(interp, str) or not (
                interp.startswith("/lib/") or interp.startswith("/usr/lib/")
            ):
                raise ValueError("Unknown target ARM32 dynamic loader")
        elif obj.get("interpreter") is not None:
            raise ValueError("Unexpected dynamic loader on bundled library")
        needed = obj.get("needed")
        if not isinstance(needed, list) or any(
            not isinstance(lib, str) or not re.fullmatch(r"[A-Za-z0-9_.+-]{1,100}", lib)
            for lib in needed
        ):
            raise ValueError("Invalid dependency manifest for: " + name)
        if len(set(needed)) != len(needed):
            raise ValueError("Repeated dependency in manifest: " + name)
        external.update(lib for lib in needed if lib not in PRIVATE_SONAMES)
    if manifest.get("external_needed") != sorted(external):
        raise ValueError("External dependency list does not match ELF declarations")
    if "libgamestream.so.4" not in objects["bin/coreelec-ng/moonlight"]["needed"]:
        raise ValueError("Missing executable dependency on libgamestream")
    if "libmoonlight-common.so.4" not in (
        objects["lib/coreelec-ng/libgamestream.so.4"]["needed"]
    ):
        raise ValueError("Missing gamestream dependency on moonlight-common")
    # libmoonlight-aml.so is resolved with dlopen(), so a DT_NEEDED edge
    # to gamestream is not required (nor present in Vortex's reference ELF).    return manifest


def native_members(bundle, allow_legacy=False):
    found = {}
    with tarfile.open(bundle, "r:gz") as tar:
        for member in tar:
            name = posixpath.normpath(member.name)
            if name in (".", ""):
                continue
            if member.isdir():
                continue
            if (name not in NATIVE_FILES | ASSETS | OPTIONAL_FILES or
                    not member.isfile() or
                    member.size > 64 * 1024 * 1024 or
                    name in found):
                raise ValueError("Unexpected or unsafe native archive entry: " + name)
            fileobj = tar.extractfile(member)
            if fileobj is None:
                raise ValueError("Cannot read native archive entry: " + name)
            data = fileobj.read()
            if len(data) != member.size:
                raise ValueError("Truncated native archive entry: " + name)
            if name in NATIVE_FILES:
                assert_arm32(data, name)
            found[name] = data

    if set(found) & NATIVE_FILES != NATIVE_FILES:
        missing = sorted(NATIVE_FILES - set(found))
        raise ValueError("Missing native Moonlight objects: " + ", ".join(missing))
    if MANIFEST in found:
        verify_runtime_manifest(found)
    elif not allow_legacy:
        raise ValueError("Missing native runtime manifest; pass --allow-legacy-bundle only for earlier test artifacts")
    return found


def versioned_addon_xml(data):
    root = ElementTree.fromstring(data)
    if root.tag != "addon" or root.get("id") != "script.luna":
        raise ValueError("Not the expected Luna addon.xml")

    version = root.get("version", "")
    parts = version.split(".")
    if len(parts) < 3 or any(not p.isdecimal() for p in parts):
        raise ValueError("Invalid Kodi addon version: " + version)
    # addon.xml is the single version source. Never silently reuse an old
    # package number or generate the same +1 version on every build.
    next_version = version
    root.set("version", next_version)
    root.set("name", "Luna (CoreELEC 21 NG test)")
    root.set("provider-name", "Vortex, Roboatlas21")
    return ElementTree.tostring(root, encoding="utf-8", xml_declaration=True), next_version


def zip_entry(zip_file, path, data, executable=False):
    info = zipfile.ZipInfo("script.luna/" + path, date_time=(2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    mode = 0o755 if executable else 0o644
    info.external_attr = (stat.S_IFREG | mode) << 16
    zip_file.writestr(info, data)


def package(source, bundle, output, allow_legacy=False, vfm_directory=None):
    source = Path(source).resolve()
    if not (source / "addon.xml").is_file():
        raise ValueError("Luna repository root must contain addon.xml")
    native = native_members(bundle, allow_legacy=allow_legacy)
    vfm_files = {}
    if vfm_directory is not None:
        spec = importlib.util.spec_from_file_location(
            'vfm_validator', source / 'resources/lib/util/vfmruntime.py')
        validator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(validator)
        root = Path(vfm_directory)
        manifest = validator.verify_bundle(root)
        for name in [*manifest['objects'], 'manifest.json']:
            vfm_files['vfm/' + name] = (root / name).read_bytes()
    addon_xml, version = versioned_addon_xml((source / "addon.xml").read_bytes())

    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if source in output.parents:
        raise ValueError("Output ZIP must be outside the source checkout")

    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as result:
        zip_entry(result, "addon.xml", addon_xml)
        # Unlike the original NO binary directory, this NG add-on does not
        # inherit a sample configuration. Luna rewrites it from Kodi settings,
        # but a safe empty placeholder ensures native get_path() can find it.
        zip_entry(result, "bin/coreelec-ng/moonlight.conf",
                  b"# Generated by Luna from Kodi settings on launch.\n[Moonlight]\n")
        for path in sorted(source.rglob("*")):
            if not path.is_file() or path.is_symlink():
                continue
            rel = path.relative_to(source)
            if (rel.as_posix() == "addon.xml" or
                    rel.parts[0] in EXCLUDED_ROOTS or
                    any(p in EXCLUDED_CACHE_DIRS for p in rel.parts[:-1])):
                continue
            if len(rel.parts) >= 3 and rel.parts[:2] == ("resources", "launchscripts"):
                if rel.parts[2] != NG_LAUNCHER:
                    continue
            if rel.suffix in (".pyc", ".pyo") or rel.name.startswith("."):
                continue
            executable = rel.suffix == ".sh"
            zip_entry(result, rel.as_posix(), path.read_bytes(), executable)
        for name, data in sorted(vfm_files.items()):
            zip_entry(result, name, data)
        for name in sorted(native):
            zip_entry(result, name, native[name],
                      executable=(name == "bin/coreelec-ng/moonlight"))

    # Verify the final ZIP includes the exact native ELF files.
    with zipfile.ZipFile(output) as result:
        for name in NATIVE_FILES:
            assert_arm32(result.read("script.luna/" + name), name)
        if result.testzip():
            raise ValueError("ZIP corruption detected")
    return version


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native_bundle", type=Path)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--allow-legacy-bundle", action="store_true",
                        help="Allow earlier unverified ELF-only test bundles (not recommended)")
    parser.add_argument('--vfm-directory', type=Path,
                        help='Optional verified ARM32 FFmpeg/VFM prototype bundle')
    args = parser.parse_args()
    version = package(args.source, args.native_bundle, args.output,
                      allow_legacy=args.allow_legacy_bundle, vfm_directory=args.vfm_directory)
    print("Created %s (Luna %s)" % (args.output, version))


if __name__ == "__main__":
    main()
