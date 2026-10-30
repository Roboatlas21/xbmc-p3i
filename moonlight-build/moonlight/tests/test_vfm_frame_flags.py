"""Check production bridge metadata against the pinned CE21 display contract."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VfmFrameFlagTests(unittest.TestCase):
    def test_linear_buffers_keep_display_order_and_streaming_timing_flags(self):
        source = (ROOT / 'kernel/moonlight-vfm/moonlight_vfm.c').read_text()
        start = source.index('  vf = &slot->vf;', source.index('static int submit_frame('))
        metadata = source[start:source.index('  vf->width =', start)]
        # Values from linux-amlogic 2d0e0cc0's media/vfm/vframe.h. The decoder
        # sets VIDEO_LINEAR before exporting a capture buffer; video_hw.c
        # uses it for GEN_REG bit 4 and GEN_REG3 bit 0. GAME_MODE independently
        # bypasses media-clock scheduling for the live stream in video.c.
        harness = r'''
#include <assert.h>
#define VFRAME_FLAG_GAME_MODE 0x20
#define VFRAME_FLAG_VIDEO_LINEAR 0x400
#define VIDTYPE_PROGRESSIVE 0
#define VIDTYPE_VIU_FIELD 0x1000
#define VIDTYPE_VIU_NV12 0x80
struct vframe_s { unsigned int index, type, type_original, flag; };
struct frame_slot { struct vframe_s vf; };
int main(void) {
  struct frame_slot storage = {0}, *slot = &storage;
  struct vframe_s *vf;
  unsigned int i = 2;
'''
        harness += metadata
        harness += r'''
  assert(vf->flag & VFRAME_FLAG_VIDEO_LINEAR);
  assert(vf->flag & VFRAME_FLAG_GAME_MODE);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'frame-flags.c').write_text(harness)
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                           ['-std=c99', '-Wall', '-Wextra', '-Werror',
                            str(path / 'frame-flags.c'), '-o', str(path / 'frame-flags')],
                           check=True)
            subprocess.run([str(path / 'frame-flags')], check=True)


if __name__ == '__main__':
    unittest.main()
