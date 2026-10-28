#!/usr/bin/env python3
"""Read-only ELF export audit of the ORIGINAL Vortex Luna ZIP.

The script checks API signatures and the platform low-latency stubs, not
instruction-level equivalence or runtime latency.
"""
import hashlib
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

EXPECTED_SHA256 = "74c93039401b146b87fb67b62f95f1054834d48fba0bb5d88c64c639b78d0837"
MEMBER = "script.luna/lib/libmoonlight-common.so.4"
REQUIRED = {
    "RtpvAddPacket",
    "queueRtpPacket",
    "LiWaitForNextVideoFrame",
    "LiGetPendingVideoFrames",
    "LiGetRTPVideoStats",
    "sendInputPacketOnControlStream",
    "enterLowLatencyMode",
    "exitLowLatencyMode",
}


def parse_symbols(raw):
    exports = {}
    for line in raw.splitlines():
        f = line.split()
        if len(f) < 8 or not f[0].endswith(":") or f[3] != "FUNC" or f[6] == "UND":
            continue
        try:
            exports[f[7].split("@", 1)[0]] = (int(f[1], 16), int(f[2]))
        except ValueError:
            continue
    return exports


def inspect(zip_path):
    if hashlib.sha256(zip_path.read_bytes()).hexdigest() != EXPECTED_SHA256:
        raise ValueError("Not the verified Vortex original ZIP: SHA-256 mismatch")
    with zipfile.ZipFile(zip_path) as archive:
        payload = archive.read(MEMBER)
    with tempfile.TemporaryDirectory(prefix="vortex-common-") as folder:
        so = Path(folder) / "libmoonlight-common.so.4"
        so.write_bytes(payload)
        symbols = parse_symbols(subprocess.check_output(["readelf", "-Ws", str(so)], text=True))
        missing = sorted(REQUIRED - set(symbols))
        if missing:
            raise ValueError("Missing expected exported symbols: " + ", ".join(missing))
        print("Original library SHA256:", hashlib.sha256(payload).hexdigest())
        print("Queue, video and input exports: present")
        for name in ("enterLowLatencyMode", "exitLowLatencyMode"):
            addr, size = symbols[name]
            print("%s: address=0x%x, size=%d bytes" % (name, addr, size))
            if size != 4:
                raise ValueError("Unexpected low-latency hook size: " + name)
            disasm = subprocess.check_output(
                ["llvm-objdump", "-d", "--no-show-raw-insn",
                 "--start-address=0x%x" % addr, "--stop-address=0x%x" % (addr + size), str(so)],
                text=True)
            if not re.search(r":\s+ret(?:\s|$)", disasm, re.MULTILINE):
                raise ValueError("Low-latency hook is not a single AArch64 ret: " + name)
    print("PASS: observed exports and no-op low-latency platform hooks")
    print("NOTE: matching exports do NOT prove identical private implementations")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: audit_vortex_common.py /path/to/script.luna-NO-22.zip")
    inspect(Path(sys.argv[1]))
