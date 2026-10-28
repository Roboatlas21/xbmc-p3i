"""Installed NG artifact smoke tests; no actual ARM binaries are executed."""
import hashlib
import json
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from resources.lib.util.runtimecheck import check_ng_runtime, NativeRuntimeError

OBJECTS = (
    "bin/coreelec-ng/moonlight",
    "lib/coreelec-ng/libmoonlight-aml.so",
    "lib/coreelec-ng/libgamestream.so.4",
    "lib/coreelec-ng/libmoonlight-common.so.4",
    "lib/coreelec-ng/libcec.so.6",
)
ASSET = "bin/coreelec-ng/gamecontrollerdb.txt"

def fake_arm():
    content = bytearray(52)
    content[:6] = b"\x7fELF\x01\x01"
    struct.pack_into("<H", content, 18, 40)
    return bytes(content)

def make_package(root):
    root = Path(root)
    objects, assets = {}, {}
    for name in OBJECTS + (ASSET,):
        content = fake_arm() if name in OBJECTS else b"# SDL controller mappings\n" * 6
        file = root / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(content)
        file.chmod(0o755 if name.endswith("moonlight") else 0o644)
        info = {"size": len(content), "sha256": hashlib.sha256(content).hexdigest()}
        if name in OBJECTS:
            info["interpreter"] = "/lib/ld-linux-armhf.so.3" if name.endswith("moonlight") else None
            objects[name] = info
        else:
            assets[name] = info
    manifest = {"schema": "luna-ng-runtime-v1",
                "target": "Amlogic-ce/Amlogic-ng/arm",
                "objects": objects, "assets": assets}
    (root / "runtime-manifest.json").write_text(json.dumps(manifest))
    return str(root / OBJECTS[0])

class RuntimePreflightTests(unittest.TestCase):
    @mock.patch("resources.lib.util.runtimecheck.subprocess.run")
    def test_valid_install_runs_help_with_private_loader_environment(self, runner):
        runner.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="Moonlight Embedded 2.7.1\n")
        with tempfile.TemporaryDirectory() as tmp:
            executable = make_package(tmp)
            env = {"LD_LIBRARY_PATH": os.path.join(tmp,"lib","coreelec-ng")}
            self.assertTrue(check_ng_runtime(tmp, executable, env, check_loader=False))
            runner.assert_called_once()
            _, kw = runner.call_args
            self.assertEqual(kw["cwd"], os.path.join(tmp,"bin","coreelec-ng"))
            self.assertEqual(kw["env"], env)

    @mock.patch("resources.lib.util.runtimecheck.subprocess.run")
    def test_tampered_library_is_rejected_before_any_process_launch(self, runner):
        with tempfile.TemporaryDirectory() as tmp:
            executable = make_package(tmp)
            with open(Path(tmp) / OBJECTS[2], "ab") as file:
                file.write(b"broken")
            with self.assertRaisesRegex(NativeRuntimeError, "checksum mismatch"):
                check_ng_runtime(tmp, executable, check_loader=False)
            runner.assert_not_called()

    @mock.patch("resources.lib.util.runtimecheck.subprocess.run")
    def test_missing_mapping_is_rejected(self, runner):
        with tempfile.TemporaryDirectory() as tmp:
            executable = make_package(tmp)
            os.remove(Path(tmp) / ASSET)
            with self.assertRaisesRegex(NativeRuntimeError, "Missing native file"):
                check_ng_runtime(tmp, executable, check_loader=False)
            runner.assert_not_called()

    @mock.patch("resources.lib.util.runtimecheck.subprocess.run")
    def test_help_linker_failure_blocks_launch(self, runner):
        runner.return_value = subprocess.CompletedProcess(
            args=[], returncode=127, stdout="libamcodec not found")
        with tempfile.TemporaryDirectory() as tmp:
            executable = make_package(tmp)
            with self.assertRaisesRegex(NativeRuntimeError, "external ARM32"):
                check_ng_runtime(tmp, executable, check_loader=False)

    @mock.patch("resources.lib.util.runtimecheck.subprocess.run")
    def test_system_binary_is_not_constrained_by_ng_bundle(self, runner):
        self.assertFalse(check_ng_runtime("/tmp/luna", "/usr/bin/moonlight"))
        runner.assert_not_called()

if __name__ == "__main__":
    unittest.main()
