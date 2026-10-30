"""Compile and fault-inject the actual Moonlight unique ID persistence routine."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r"""
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define UNIQUEID_CHARS 16
#define UNIQUE_FILE_NAME "uniqueid.dat"
#define GS_OK 0
#define GS_FAILED -1
static char unique_id[UNIQUEID_CHARS+1];
const char* gs_error;
static int fail_sync;
static int tested_fsync(int fd) {
  if (fail_sync) { errno = ENOSPC; return -1; }
  return fsync(fd);
}
#define fsync tested_fsync
"""

MAIN = r"""
#undef fsync
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  fail_sync = strcmp(argv[2], "fail") == 0;
  int status = load_unique_id(argv[1]);
  if (status != GS_OK) {
    fprintf(stderr, "failure: %s\n", gs_error ? gs_error : "unknown");
    return 1;
  }
  printf("%s\n", unique_id);
  return 0;
}
"""


class UniqueIdPersistenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc"):
            raise unittest.SkipTest("C compiler unavailable")
        cls.workspace = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.workspace.cleanup)
        root = Path(cls.workspace.name)
        source = (ROOT / "libgamestream/client.c").read_text()
        start = source.index("static int read_existing_unique_id(")
        end = source.index("\nstatic int load_cert(", start)
        harness = root / "id.c"
        harness.write_text(PRELUDE + source[start:end] + MAIN)
        cls.binary = root / "test-id"
        output = subprocess.run(
            ["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
             str(harness), "-o", str(cls.binary)],capture_output=True,text=True)
        if output.returncode:
            raise AssertionError(output.stderr)

    def test_initial_save_and_reuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([str(self.binary), tmp, "ok"],
                                    capture_output=True,text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((Path(tmp)/"uniqueid.dat").read_bytes(),
                             b"0123456789ABCDEF")
            self.assertEqual((Path(tmp)/"uniqueid.dat").stat().st_mode & 0o777,
                             0o600)
            again = subprocess.run([str(self.binary), tmp, "fail"],
                                   capture_output=True,text=True)
            self.assertEqual(again.returncode, 0, again.stderr)

    def test_failed_sync_leaves_no_published_id(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([str(self.binary), tmp, "fail"],
                                    capture_output=True,text=True)
            self.assertEqual(result.returncode, 1)
            self.assertFalse((Path(tmp)/"uniqueid.dat").exists())
            self.assertEqual(list(Path(tmp).glob(".uniqueid.*")), [])

    def test_invalid_hex_is_not_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "uniqueid.dat"
            path.write_bytes(b"123456789012345Z")
            result = subprocess.run([str(self.binary), directory, "ok"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)

    def test_nonregular_id_and_symlink_do_not_block_or_become_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "uniqueid.dat"
            import os
            os.mkfifo(path)
            result = subprocess.run([str(self.binary), tmp, "ok"],
                                    capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 1, result.stderr)
            path.unlink()
            real = Path(tmp) / "other-id"
            real.write_bytes(b"0123456789ABCDEF")
            path.symlink_to(real)
            result = subprocess.run([str(self.binary), tmp, "ok"],
                                    capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 1, result.stderr)
            path.unlink()
            path.mkdir()
            result = subprocess.run([str(self.binary), tmp, "ok"],
                                    capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 1, result.stderr)

    def test_invalid_persisted_id_is_not_silently_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"uniqueid.dat"
            path.write_bytes(b"partial")
            result = subprocess.run([str(self.binary), tmp, "ok"],
                                    capture_output=True,text=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(path.read_bytes(), b"partial")


if __name__ == "__main__":
    unittest.main()
