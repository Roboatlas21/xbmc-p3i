"""Compile production axis scaling with malformed input geometry under UBSan."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

SOURCE = pathlib.Path(__file__).resolve().parents[1] / "src/input/evdev.c"


class AxisGeometryTests(unittest.TestCase):
    def test_sticks_and_triggers_never_divide_by_zero(self):
        if not shutil.which("cc"):
            self.skipTest("C compiler unavailable")
        text = SOURCE.read_text()
        start = text.index("static bool evdev_axis_usable(")
        end = text.index("\nvoid *HandleMouseEmulation(", start)
        code = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
struct input_event { int code, value; };
struct input_device { int unused; };
struct input_abs_parms {int min,max,flat,avg,range,diff;};
""" + text[start:end] + r"""
int main(void) {
  struct input_event ev = {0, 0};
  struct input_device dev = {0};
  struct input_abs_parms p = {.min=0,.max=10,.flat=5,.avg=5,.diff=10};
  assert(evdev_convert_value(&ev,&dev,&p,false) == 0);
  assert(evdev_convert_value_byte(&ev,&dev,&p,0) == 0);
  p.max=0; p.flat=0;
  assert(evdev_convert_value(&ev,&dev,&p,false) == 0);
  p.max=255; p.flat=4; p.avg=127; p.diff=255;
  assert(evdev_convert_value(&ev,&dev,&p,false) <= 0);
  assert(evdev_convert_value_byte(&ev,&dev,&p,0) == 0);
  ev.value=255;
  assert(evdev_convert_value_byte(&ev,&dev,&p,0) == 255);
  p.min=-32768; p.max=32767; p.avg=0; p.flat=0; p.diff=65535;
  ev.value=32767;
  assert(evdev_convert_value_byte(&ev,&dev,&p,'+') == 255);
  assert(evdev_convert_value_byte(&ev,&dev,&p,'-') == 0);
  ev.value=-32768;
  assert(evdev_convert_value_byte(&ev,&dev,&p,'-') == 255);
  assert(evdev_convert_value_byte(&ev,&dev,&p,'+') == 0);
  ev.value=0;
  assert(evdev_convert_value_byte(&ev,&dev,&p,'-') == 0);
  assert(evdev_convert_value_byte(&ev,&dev,&p,'+') == 0);
  ev.value=32767;
  assert(evdev_convert_value_byte(&ev,&dev,&p,'~') == 0);
  ev.value=-32768;
  assert(evdev_convert_value_byte(&ev,&dev,&p,'~') == 255);
  p.min=INT_MIN; p.max=INT_MAX; p.flat=1;
  assert(evdev_convert_value(&ev,&dev,&p,false) == 0);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "axis.c").write_text(code)
            binary = root / "axis"
            subprocess.run(["cc","-std=gnu99","-Wall","-Wextra","-Werror",
                            "-fsanitize=undefined","-fno-sanitize-recover=undefined",
                            str(root / "axis.c"), "-o",str(binary)], check=True)
            subprocess.run([str(binary)],check=True)


if __name__ == "__main__":
    unittest.main()
