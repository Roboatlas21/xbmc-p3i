#!/usr/bin/env bash
# Check Pannal CoreELEC's actual install_pkg layout and reject build-tree
# libraries even when they have identical basenames. Host-only fixtures.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
collector="$repo/packaging/coreelec-ng/collect-native.sh"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
ce="$scratch/CoreELEC"
build="$ce/build.CoreELEC-Amlogic-ng.arm-21"
pkg="moonlight-embedded-2.7.1-ng-local"
install="$build/install_pkg/$pkg"
cec_install="$build/install_pkg/libcec-6.0.2"
mkdir -p "$install/usr/bin" "$install/usr/lib" "$cec_install/usr/lib" \
         "$build/build/$pkg" "$build/image/system/usr/bin" \
         "$build/image/system/usr/lib"
printf 'INFO_PKG_NAME="moonlight-embedded"\n' > "$install/.libreelec-package"
printf 'INFO_PKG_NAME="libcec"\n' > "$cec_install/.libreelec-package"

# Construct minimal armv7 ELF headers for a mocked readelf. The real collector
# still checks each ELF header, SONAME and dependencies in manifest generation.
python3 - "$install" "$cec_install" "$build" <<'PY'
import pathlib
import sys
installed, cec_install, build = (pathlib.Path(p) for p in sys.argv[1:])
items = {
    "usr/bin/moonlight": 16,
    "usr/lib/libmoonlight-aml.so": 17,
    "usr/lib/libgamestream.so.4": 18,
    "usr/lib/libmoonlight-common.so.4": 19,
}
for rel, marker in items.items():
    blob = bytearray(52)
    blob[:6] = b"\x7fELF\x01\x01"
    blob[16:18] = (2 if rel.startswith("usr/bin/") else 3).to_bytes(2, "little")
    blob[18:20] = (40).to_bytes(2, "little")
    blob[50] = marker
    dest = installed / rel
    dest.write_bytes(blob)
    # Decoy in OS staging must never be collected.
    decoy = build / "image/system" / rel
    decoy.write_bytes(bytes(blob[:50]) + b"\xff" + blob[51:])

cec = bytearray(52)
cec[:6] = b"\x7fELF\x01\x01"
cec[16:18] = (3).to_bytes(2, "little")
cec[18:20] = (40).to_bytes(2, "little")
cec[50] = 20
(cec_install / "usr/lib/libcec.so.6.0.2").write_bytes(cec)
PY

mkdir -p "$scratch/mocks"
cat > "$scratch/mocks/readelf" <<'MOCK'
#!/usr/bin/env bash
set -e
case "$1" in
  -h)
    printf '  Class:                             ELF32\n  Machine:                           ARM\n'
    ;;
  -l)
    printf '      [Requesting program interpreter: /lib/ld-linux-armhf.so.3]\n'
    ;;
  -d)
    case "${2##*/}" in
      moonlight)
        printf ' (NEEDED) Shared library: [libgamestream.so.4]\n'
        printf ' (NEEDED) Shared library: [libc.so.6]\n' ;;
      libmoonlight-aml.so)
        printf ' (SONAME) Library soname: [libmoonlight-aml.so]\n'
        printf ' (NEEDED) Shared library: [libamcodec.so]\n' ;;
      libgamestream.so.4)
        printf ' (SONAME) Library soname: [libgamestream.so.4]\n'
        printf ' (NEEDED) Shared library: [libmoonlight-common.so.4]\n' ;;
      libmoonlight-common.so.4)
        printf ' (SONAME) Library soname: [libmoonlight-common.so.4]\n'
        printf ' (NEEDED) Shared library: [libc.so.6]\n' ;;
      libcec.so.6)
        printf ' (SONAME) Library soname: [libcec.so.6]\n'
        printf ' (NEEDED) Shared library: [libudev.so.1]\n'
        printf ' (NEEDED) Shared library: [libc.so.6]\n' ;;
      *) exit 1 ;;
    esac
    ;;
  *) exit 2 ;;
esac
MOCK
chmod +x "$scratch/mocks/readelf"
# The collector intentionally uses real file, sha256sum, tar, and Python,
# but a deterministic readelf keeps this host test cross-toolchain-free.
export PATH="$scratch/mocks:$PATH"
if env -u SOURCE_GIT_SHA bash "$collector" "$ce" "$scratch/no-revision" \
    > "$scratch/no-revision.log" 2>&1; then
  echo "Collector accepted a missing source revision" >&2
  exit 1
fi
grep -Fq "SOURCE_GIT_SHA must be a full lowercase 40-hex" "$scratch/no-revision.log"
if SOURCE_GIT_SHA=unknown bash "$collector" "$ce" "$scratch/unknown" \
    > "$scratch/unknown.log" 2>&1; then
  echo "Collector accepted unknown source revision" >&2
  exit 1
fi
export SOURCE_GIT_SHA="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
mkdir -p "$scratch/output"
bash "$collector" "$ce" "$scratch/output" > "$scratch/success.log"
grep -Fq "Collecting relocatable installed binaries from: $install" "$scratch/success.log"
python3 - "$scratch/output/moonlight-coreelec21-ng-arm32.tar.gz" <<'PY'
import hashlib
import json
import tarfile
import sys
with tarfile.open(sys.argv[1], "r:gz") as tar:
    def item(name):
        return tar.extractfile("./" + name).read()
    data = item("bin/coreelec-ng/moonlight")
    assert data[50] == 16, "collector took a decoy instead of installed binary"
    manifest = json.loads(item("runtime-manifest.json"))
    assert manifest["objects"]["bin/coreelec-ng/moonlight"]["sha256"] == hashlib.sha256(data).hexdigest()
    assert manifest["source_git_sha"] == "a"*40
    assert manifest["target"] == "Amlogic-ce/Amlogic-ng/arm"
    assert "libamcodec.so" in manifest["external_needed"]
    mapping = item("bin/coreelec-ng/gamecontrollerdb.txt")
    assert len(mapping) > 100
    assert manifest["assets"]["bin/coreelec-ng/gamecontrollerdb.txt"]["sha256"] == hashlib.sha256(mapping).hexdigest()
    libcec = item("lib/coreelec-ng/libcec.so.6")
    assert libcec[50] == 20
    assert manifest["objects"]["lib/coreelec-ng/libcec.so.6"]["soname"] == "libcec.so.6"
    assert "libudev.so.1" in manifest["external_needed"]
PY

# Missing Moonlight install objects must fail, even when decoy binaries and
# source build outputs exist elsewhere.
rm -f "$install/usr/lib/libgamestream.so.4"
if bash "$collector" "$ce" "$scratch/missing" > "$scratch/failure.log" 2>&1; then
  echo "Collector accepted a missing installed Moonlight library" >&2
  exit 1
fi
grep -Fq "Missing ARM ELF32 shared object: libgamestream.so.4" "$scratch/failure.log"
echo "PASS: install_pkg collector, correct ARM ELF provenance, manifest, and missing-library rejection"
