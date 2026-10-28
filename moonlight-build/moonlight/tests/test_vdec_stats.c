/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/osd/stats.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *h264 =
  "vdec channel 0 statistics:\n"
  "  device name : amvdec_h264\n"
  "  frame width : 1920\n"
  " frame height : 1080\n"
  "   frame rate : 60 fps\n"
  "      status : 63\n"
  "  frame count : 100\n"
  "   drop count : 2\n"
  "fra err count : 1\n"
  " hw err count : 0\n";

static const char *hevc =
  "vdec channel 1 statistics:\n"
  "  device name : ammvdec_hevc\n"
  "  frame width : 3840\n"
  " frame height : 2160\n"
  " frame count : 130\n"
  " drop count : 4\n"
  " fra err count : 2\n"
  " hw err count : 1\n";

int main(void) {
  OsdVdecCounters sample = {0};
  assert(osd_vdec_parse(h264, 1920, 1080, 1, &sample));
  assert(sample.channel == 0 && sample.frame_count == 100 &&
         sample.drop_count == 2 && sample.frame_errors == 1 &&
         sample.hw_errors == 0 && sample.width == 1920);
  assert(!osd_vdec_parse(h264, 3840, 2160, 1, &sample));
  assert(!osd_vdec_parse(h264, 1920, 1080, 2, &sample));
  assert(!osd_vdec_parse("No vdec.\n", 1920, 1080, 1, &sample));
  assert(!osd_vdec_parse("vdec channel 0 statistics:\n  device name : amvdec_h264\n"
                         "frame count : 12\n", 1920,1080,1,&sample));
  char joined[1500];
  snprintf(joined, sizeof(joined), "%s%s", h264, hevc);
  assert(osd_vdec_parse(joined, 3840, 2160, 2, &sample));
  assert(sample.channel == 1 && sample.frame_count == 130);
  snprintf(joined, sizeof(joined), "%s%s", h264, h264);
  assert(!osd_vdec_parse(joined, 1920, 1080, 1, &sample));
  assert(!osd_vdec_parse(joined, 1920, 1080, 0, &sample));

  OsdTracker tracker = {0};
  OsdSnapshot out = {0};
  sample = (OsdVdecCounters){.channel=0,.width=1920,.height=1080,
    .codec_family=1,.frame_count=100,.drop_count=10,
    .frame_errors=2,.hw_errors=1};
  osd_tracker_decoder_update(&tracker, 1000, &sample, &out);
  assert(!out.has_decoder_fps && !out.has_decoder_deltas);
  sample.frame_count=160;
  sample.drop_count=12;
  sample.frame_errors=3;
  sample.hw_errors=1;
  out=(OsdSnapshot){0};
  osd_tracker_decoder_update(&tracker, 2000, &sample, &out);
  assert(out.has_decoder_fps && fabsf(out.decoder_fps-60.0f) < .01f);
  assert(out.has_decoder_deltas && out.decoder_drops == 2 &&
         out.decoder_frame_errors == 1 && out.decoder_hw_errors == 0);
  sample.frame_count=3; sample.drop_count=0; /* decoder reset */
  out=(OsdSnapshot){0};
  osd_tracker_decoder_update(&tracker,3000,&sample,&out);
  assert(!out.has_decoder_fps && !out.has_decoder_deltas);
  sample.frame_count=63;
  out=(OsdSnapshot){0};
  osd_tracker_decoder_update(&tracker,4000,&sample,&out);
  assert(out.has_decoder_fps && fabsf(out.decoder_fps-60.0f)<.01f);
  sample.channel=1; /* decoder instance switched */
  out=(OsdSnapshot){0};
  osd_tracker_decoder_update(&tracker,5000,&sample,&out);
  assert(!out.has_decoder_fps);
  osd_tracker_decoder_reset(&tracker);
  out=(OsdSnapshot){0};
  osd_tracker_decoder_update(&tracker,6000,&sample,&out);
  assert(!out.has_decoder_fps);
  printf("PASS: vdec channel filtering, reset, decoder FPS and error deltas\n");
  return 0;
}
