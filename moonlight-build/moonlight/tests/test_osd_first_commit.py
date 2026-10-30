"""Ensure a failed first atomic DRM commit cannot clear Kodi's attached plane."""
from pathlib import Path
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / 'src/osd/drm.c').read_text()


class OsdFirstCommitTests(unittest.TestCase):
    def test_first_failure_preserves_prior_plane(self):
        self.assertIn('bool plane_committed;', SOURCE)
        self.assertIn('osd_plane_disable_needed(s->owns_plane, s->plane_committed) && !set_plane(s,0)', SOURCE)
        start = SOURCE.index('bool osd_drm_draw(')
        tail = SOURCE[start:]
        self.assertLess(tail.index('if(!set_plane(s,b->fb))return false;'),
                        tail.index('s->plane_committed=true;'))
        self.assertLess(tail.index('s->plane_committed=true;'),
                        tail.index('s->visible=index;'))


if __name__ == '__main__':
    unittest.main()
