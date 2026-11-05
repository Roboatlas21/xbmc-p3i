#!/usr/bin/env python3
"""Compile and execute the exact helper embedded in the kernel patch."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def extract_new_headers(patch, destination):
    """Only extract complete new-file hunks, never approximate patched sources."""
    wanted = {
        "drivers/amlogic/media/enhancement/amdolby_vision/dv_l5_transform.h",
        "include/linux/amlogic/media/amdolbyvision/dv_l5_geometry.h",
    }
    found = set()
    for section in patch.split("diff --git ")[1:]:
        lines = section.splitlines(True)
        target = lines[0].strip().split(" b/", 1)[1]
        if target not in wanted:
            continue
        if "new file mode 100644\n" not in lines:
            raise RuntimeError(f"Expected a complete new header: {target}")
        content = "".join(line[1:] for line in lines if line.startswith("+") and not line.startswith("+++"))
        file = destination / target
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(content, encoding="utf-8", newline="\n")
        found.add(target)
    if found != wanted:
        raise RuntimeError(f"Missing new headers: {wanted - found}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"), help="compiler command, e.g. 'clang' or 'zig cc'")
    parser.add_argument("--cflags", default="", help="extra flags, e.g. --cflags=-fsanitize=address,undefined")
    parser.add_argument("--compile-only", action="store_true", help="cross-compile the test translation unit without running it")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    patch = (here.parent / "linux-990-dv-l5-display-geometry.patch").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="p3i-l5-") as temp:
        root = Path(temp)
        extract_new_headers(patch, root)
        binary = root / ("test_l5.o" if args.compile_only else "test_l5.exe" if os.name == "nt" else "test_l5")
        command = shlex.split(args.cc) + ["-std=gnu89", "-O2", "-Wall", "-Wextra", "-Werror"]
        command += shlex.split(args.cflags)
        command += ["-I" + str(here / "include"), "-I" + str(root / "include"), "-I" + str(root / "drivers/amlogic/media/enhancement/amdolby_vision")]
        if args.compile_only:
            command += ["-c"]
        subprocess.run(command + [str(here / "test_l5.c"), "-o", str(binary)], check=True)
        if args.compile_only:
            print("PASS: helper cross-compiled with warnings treated as errors")
        else:
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
