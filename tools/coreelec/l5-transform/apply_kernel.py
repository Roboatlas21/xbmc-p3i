#!/usr/bin/env python3
"""Validate and apply the pinned P3i L5 patch to an unpacked kernel tree."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", type=Path, help="unpacked, unmodified pinned kernel source")
    parser.add_argument("--check", action="store_true", help="validate without modifying files")
    parser.add_argument("--git", default="git", help="git executable")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    kernel = args.kernel.resolve(strict=True)
    manifest = json.loads((here / "kernel-base.json").read_text(encoding="utf-8"))
    patch = here / "linux-990-dv-l5-display-geometry.patch"
    for name, expected in manifest["preimage_sha256"].items():
        file = kernel / name
        if expected is None:
            if file.exists():
                raise SystemExit(f"Refusing to overwrite existing file: {file}")
        elif not file.is_file() or hashlib.sha256(file.read_bytes()).hexdigest() != expected:
            raise SystemExit(f"Unexpected source: {file}\nExpected {manifest['repository']} at {manifest['commit']}. No files changed.")
    command = [args.git, "-c", "core.autocrlf=false", "apply", "--whitespace=error"]
    subprocess.run(command + ["--check", str(patch)], cwd=kernel, check=True)
    if args.check:
        print("PASS: all pinned preimages match; patch applies cleanly")
    else:
        subprocess.run(command + [str(patch)], cwd=kernel, check=True)
        print("Applied L5 display-geometry patch. Rebuild the kernel and matching modules.")


if __name__ == "__main__":
    main()
