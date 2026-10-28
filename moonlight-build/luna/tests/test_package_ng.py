"""Validate the native-bundle safety checks and Kodi ZIP file layout."""
import hashlib
import importlib.util
import io
import json
import stat
import struct
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from xml.etree import ElementTree

SCRIPT = Path(__file__).resolve().parents[1] / "packaging" / "package_ng.py"
spec = importlib.util.spec_from_file_location("luna_ng_packaging", SCRIPT)
package_ng = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_ng)


def fake_elf(elf_class=1, machine=40):
    data = bytearray(64)
    data[:4] = bytes((127,)) + b"ELF"
    data[4] = elf_class
    data[5] = 1
    struct.pack_into("<H", data, 18, machine)
    return bytes(data)


def write_bundle(path, *, elf_class=1, omit=None, bad_entry=None,
                 wrong_digest=False, no_manifest=False):
    with tarfile.open(path, "w:gz") as tar:
        objects = {}
        for name in sorted(package_ng.NATIVE_FILES):
            if name == omit:
                continue
            content = fake_elf(elf_class)
            item = tarfile.TarInfo("./" + name)
            item.mode = 0o755
            item.size = len(content)
            tar.addfile(item, io.BytesIO(content))
            expected = package_ng.EXPECTED_LIB_SONAMES.get(name)
            needed = (["libgamestream.so.4", "libc.so.6"] if name.startswith("bin/")
                      else ["libamcodec.so", "libc.so.6"] if name.endswith("libmoonlight-aml.so")
                      else ["libmoonlight-common.so.4"] if name.endswith("libgamestream.so.4")
                      else ["libc.so.6"])
            objects[name] = {"class": "ELF32", "machine": "ARM",
                             "size": len(content),
                             "sha256": hashlib.sha256(content).hexdigest(),
                             "needed": needed, "soname": expected,
                             "interpreter": "/lib/ld-linux-armhf.so.3" if name.startswith("bin/")
                             else None, "e_flags": 0}
        if wrong_digest:
            objects["bin/coreelec-ng/moonlight"]["sha256"] = "f" * 64
        if not no_manifest:
            manifest = {
                "schema": package_ng.SCHEMA,
                "source_git_sha": "a" * 40,
                "target": "Amlogic-ce/Amlogic-ng/arm",
                "objects": objects,
                "assets": {
                    "bin/coreelec-ng/gamecontrollerdb.txt": {
                        "size": len(("# SDL mappings\n" + "03000000000000000000000000000000,Test Controller,a:b0\n"*5).encode()),
                        "sha256": hashlib.sha256(("# SDL mappings\n" + "03000000000000000000000000000000,Test Controller,a:b0\n"*5).encode()).hexdigest(),
                    }
                },
                "external_needed": ["libamcodec.so", "libc.so.6"],
                "external_library_check": "not verified on target device",
            }
            content = json.dumps(manifest, sort_keys=True).encode("utf-8")
            item = tarfile.TarInfo("./runtime-manifest.json")
            item.size = len(content)
            tar.addfile(item, io.BytesIO(content))
        if not no_manifest:
            map_name = "bin/coreelec-ng/gamecontrollerdb.txt"
            data = ("# SDL mappings\n" + "03000000000000000000000000000000,Test Controller,a:b0\n"*5).encode()
            item = tarfile.TarInfo("./" + map_name)
            item.size = len(data)
            tar.addfile(item, io.BytesIO(data))
        if bad_entry:
            item = tarfile.TarInfo(bad_entry)
            item.size = 2
            tar.addfile(item, io.BytesIO(b"ok"))


class KodiPackageTests(unittest.TestCase):
    def test_package_preserves_layout_and_executable_bit(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            source = base / "luna"
            source.mkdir()
            (source / "addon.xml").write_text(
                '<addon id="script.luna" name="Luna" version="2.7.1.302" '
                'provider-name="Vortex"><requires /></addon>')
            (source / "addon.py").write_text("print('hello')\n")
            (source / "resources").mkdir()
            (source / "resources" / "test.txt").write_text("test")
            # A nested 'lib' is a Kodi Python module directory, NOT the
            # top-level native lib directory excluded from the source walk.
            internal = source / "resources" / "lib" / "util"
            internal.mkdir(parents=True)
            (internal / "helper.py").write_text("value = 42\n")
            safe = source / "resources" / "launchscripts" / "coreelec"
            safe.mkdir(parents=True)
            (safe / "launch.sh").write_text("#!/bin/sh\nexit 0\n")
            (safe / "kodi-watchdog.sh").write_text("#!/bin/sh\nexit 0\n")
            unsafe = source / "resources" / "launchscripts" / "coreelec_usb_new"
            unsafe.mkdir(parents=True)
            (unsafe / "launch.sh").write_text(
                "#!/bin/sh\necho 0 > /sys/bus/usb/devices/1-1/authorized\n")
            (source / ".github").mkdir()
            (source / ".github" / "ignored.yml").write_text("secret")
            bundle = base / "native.tar.gz"
            output = base / "luna-ng.zip"
            write_bundle(bundle)
            version = package_ng.package(source, bundle, output)
            self.assertEqual(version, "2.7.1.302")

            with zipfile.ZipFile(output) as package:
                names = set(package.namelist())
                self.assertIn("script.luna/addon.py", names)
                self.assertIn("script.luna/bin/coreelec-ng/gamecontrollerdb.txt", names)
                self.assertEqual(
                    package.read("script.luna/bin/coreelec-ng/moonlight.conf"),
                    b"# Generated by Luna from Kodi settings on launch.\n[Moonlight]\n")
                manifest_name = "script.luna/runtime-manifest.json"
                self.assertIn(manifest_name, names)
                self.assertEqual(
                    json.loads(package.read(manifest_name))["schema"],
                    package_ng.SCHEMA)
                self.assertIn("script.luna/resources/test.txt", names)
                self.assertIn("script.luna/resources/lib/util/helper.py", names)
                self.assertNotIn("script.luna/.github/ignored.yml", names)
                safe_launcher = "script.luna/resources/launchscripts/coreelec/launch.sh"
                self.assertIn(safe_launcher, names)
                self.assertIn(
                    "script.luna/resources/launchscripts/coreelec/kodi-watchdog.sh",
                    names)
                self.assertNotIn(
                    "script.luna/resources/launchscripts/coreelec_usb_new/launch.sh",
                    names)
                self.assertEqual(
                    (package.getinfo(safe_launcher).external_attr >> 16) & 0o777,
                    0o755)
                self.assertTrue(package.read(safe_launcher).startswith(b"#!/bin/sh"))
                binary = "script.luna/bin/coreelec-ng/moonlight"
                self.assertIn(binary, names)
                self.assertEqual((package.getinfo(binary).external_attr >> 16) & 0o777, 0o755)
                addon = ElementTree.fromstring(package.read("script.luna/addon.xml"))
                self.assertEqual(addon.get("version"), "2.7.1.302")
                self.assertEqual(addon.get("id"), "script.luna")

    def test_real_luna_checkout_package_handoff(self):
        """Assemble the actual repository layout using synthetic ARM32 objects."""
        with tempfile.TemporaryDirectory() as tmp:
            root = SCRIPT.parents[1]
            bundle = Path(tmp) / "native.tar.gz"
            output = Path(tmp) / "luna-ng-installable.zip"
            write_bundle(bundle)
            package_ng.package(root, bundle, output)
            with zipfile.ZipFile(output) as archive:
                names = set(archive.namelist())
                required = {
                    "script.luna/addon.xml",
                    "script.luna/addon.py",
                    "script.luna/resources/launchscripts/coreelec/tsync.sh",
                    "script.luna/resources/settings.xml",
                    "script.luna/resources/lib/core/logger.py",
                    "script.luna/resources/lib/util/audiodevices.py",
                    "script.luna/resources/lib/util/confighelper.py",
                    "script.luna/resources/lib/util/moonlighthelper.py",
                    "script.luna/resources/lib/util/runtimecheck.py",
                    "script.luna/resources/launchscripts/coreelec/launch.sh",
                    "script.luna/resources/launchscripts/coreelec/kodi-watchdog.sh",
                    "script.luna/resources/launchscripts/coreelec/prescript.sh",
                    "script.luna/resources/launchscripts/coreelec/postscript.sh",
                    "script.luna/bin/coreelec-ng/moonlight",
                    "script.luna/bin/coreelec-ng/moonlight.conf",
                    "script.luna/bin/coreelec-ng/gamecontrollerdb.txt",
                    "script.luna/lib/coreelec-ng/libmoonlight-aml.so",
                    "script.luna/lib/coreelec-ng/libgamestream.so.4",
                    "script.luna/lib/coreelec-ng/libmoonlight-common.so.4",
                    "script.luna/lib/coreelec-ng/libcec.so.6",
                    "script.luna/runtime-manifest.json",
                }
                self.assertTrue(required <= names, sorted(required - names))
                for name in required:
                    if name.endswith(".sh"):
                        self.assertTrue(archive.read(name).startswith(b"#!/bin/sh"))
                        self.assertEqual(
                            (archive.getinfo(name).external_attr >> 16) & 0o777, 0o755)
                self.assertNotIn(
                    "script.luna/resources/launchscripts/coreelec_usb_new/prescript.sh",
                    names)
                self.assertNotIn(
                    "script.luna/resources/launchscripts/coreelec_praxis/launch.sh",
                    names)
                self.assertIsNone(archive.testzip())

    def test_rejects_aarch64_no_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp) / "native.tar.gz"
            write_bundle(bundle, elf_class=2)
            with self.assertRaisesRegex(ValueError, "ARM ELF"):
                package_ng.native_members(bundle)

    def test_rejects_incomplete_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp) / "native.tar.gz"
            write_bundle(bundle, omit="lib/coreelec-ng/libmoonlight-aml.so")
            with self.assertRaisesRegex(ValueError, "Missing native"):
                package_ng.native_members(bundle)

    def test_rejects_bundle_without_libcec_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp) / "native.tar.gz"
            write_bundle(bundle, omit="lib/coreelec-ng/libcec.so.6")
            with self.assertRaisesRegex(ValueError, "Missing native"):
                package_ng.native_members(bundle)

    def test_rejects_mismatched_native_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp) / "native.tar.gz"
            write_bundle(bundle, wrong_digest=True)
            with self.assertRaisesRegex(ValueError, "digest mismatch"):
                package_ng.native_members(bundle)

    def test_manifest_required_except_explicit_legacy_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp) / "native.tar.gz"
            write_bundle(bundle, no_manifest=True)
            with self.assertRaisesRegex(ValueError, "Missing native runtime manifest"):
                package_ng.native_members(bundle)
            self.assertEqual(
                set(package_ng.native_members(bundle, allow_legacy=True)),
                package_ng.NATIVE_FILES)

    def test_rejects_unexpected_archive_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp) / "native.tar.gz"
            write_bundle(bundle, bad_entry="../escape")
            with self.assertRaisesRegex(ValueError, "unsafe"):
                package_ng.native_members(bundle)


if __name__ == "__main__":
    unittest.main()
