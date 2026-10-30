"""Pure parser tests, without cross-compilation or executing ARM binaries."""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "packaging" / "coreelec-ng"))
import runtime_manifest as manifest

class RuntimeManifestTests(unittest.TestCase):
    def test_unknown_or_invalid_source_revision_is_rejected_before_build(self):
        for invalid in ("unknown", "", "A" * 40, "g" * 40, None):
            with self.subTest(revision=invalid), \
                 self.assertRaisesRegex(ValueError, "source revision"):
                manifest.build_manifest(Path("/not-a-build"), invalid)

    def test_header_rejects_arch_and_abi_mismatch(self):
        arm = bytearray(52)
        arm[:6] = b"\x7fELF\x01\x01"
        arm[16:20] = (2).to_bytes(2, "little") + (40).to_bytes(2, "little")
        self.assertEqual(manifest.elf_properties(arm, "moonlight")["machine"], "ARM")
        arm[18:20] = (183).to_bytes(2, "little")
        with self.assertRaisesRegex(ValueError, "Wrong ARM"):
            manifest.elf_properties(arm, "aarch64")
        arm[18:20] = (40).to_bytes(2, "little")
        arm[4] = 2
        with self.assertRaisesRegex(ValueError, "ELF32"):
            manifest.elf_properties(arm, "elf64")

    def test_dynamic_dependency_and_loader_parser(self):
        output = """
 0x00000001 (NEEDED) Shared library: [libgamestream.so.4]
 0x00000001 (NEEDED) Shared library: [libc.so.6]
 0x0000000e (SONAME) Library soname: [libmoonlight-aml.so]
"""
        needed, soname = manifest.elf_dynamic(output)
        self.assertEqual(needed, ["libc.so.6", "libgamestream.so.4"])
        self.assertEqual(soname, "libmoonlight-aml.so")
        self.assertEqual(manifest.program_interpreter(
            "[Requesting program interpreter: /lib/ld-linux-armhf.so.3]"),
            "/lib/ld-linux-armhf.so.3")
        self.assertIsNone(manifest.program_interpreter("no interp"))

    def test_complete_manifest_assets_and_loader(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            def elf(typ):
                content = bytearray(52)
                content[:6] = b"\x7fELF\x01\x01"
                content[16:18] = typ.to_bytes(2, "little")
                content[18:20] = (40).to_bytes(2, "little")
                return bytes(content)
            for name in manifest.OBJECTS:
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(elf(2 if name.startswith("bin/") else 3))
            for name in manifest.ASSETS:
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b"# SDL mappings\n" * 12)

            def fake_readelf(flag, path):
                basename = Path(path).name
                if flag == "-l":
                    return "[Requesting program interpreter: /lib/ld-linux-armhf.so.3]"
                soname = "" if basename == "moonlight" else (
                    " (SONAME) Library soname: [" + basename + "]\n")
                needed = {
                    "moonlight": ["libgamestream.so.4", "libc.so.6"],
                    "libmoonlight-aml.so": ["libamcodec.so", "libc.so.6"],
                    "libgamestream.so.4": ["libmoonlight-common.so.4", "libc.so.6"],
                    "libmoonlight-common.so.4": ["libc.so.6"],
                    "libcec.so.6": ["libudev.so.1", "libc.so.6"]
                }[basename]
                return soname + "".join(
                    " (NEEDED) Shared library: [" + n + "]\n" for n in needed)

            with mock.patch.object(manifest, "readelf", side_effect=fake_readelf):
                result = manifest.build_manifest(root, "a" * 40)
                self.assertEqual(result["schema"], manifest.SCHEMA)
                self.assertEqual(set(result["assets"]), set(manifest.ASSETS))
                self.assertEqual(result["external_needed"],
                                 ["libamcodec.so", "libc.so.6", "libudev.so.1"])
                (root / manifest.ASSETS[0]).unlink()
                with self.assertRaisesRegex(ValueError, "input mapping"):
                    manifest.build_manifest(root, "a" * 40)
                (root / manifest.ASSETS[0]).write_bytes(b"# SDL mappings\n" * 12)
                def rpath_readelf(flag, path):
                    original = fake_readelf(flag, path)
                    if flag == "-d" and Path(path).name == "moonlight":
                        return original + " (RUNPATH) Library runpath: [/tmp/build-only]\n"
                    return original
                with mock.patch.object(manifest, "readelf", side_effect=rpath_readelf):
                    with self.assertRaisesRegex(ValueError, "search paths"):
                        manifest.build_manifest(root, "a" * 40)

    def test_object_names_match_collector_contract(self):
        self.assertEqual(len(manifest.OBJECTS), 5)
        self.assertEqual(manifest.ASSETS, ("bin/coreelec-ng/gamecontrollerdb.txt",))
        self.assertIn("bin/coreelec-ng/moonlight", manifest.OBJECTS)
        self.assertEqual(manifest.SONAMES["libmoonlight-common.so.4"],
                         "libmoonlight-common.so.4")
        self.assertEqual(manifest.SONAMES["libcec.so.6"], "libcec.so.6")

if __name__ == "__main__":
    unittest.main()
