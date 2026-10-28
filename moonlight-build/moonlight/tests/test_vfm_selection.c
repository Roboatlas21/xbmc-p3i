/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../src/platform.h"
#include <assert.h>
#include <string.h>

void loop_request_stop(void) {}
void write_bool(char *path, bool value) { (void)path; (void)value; }

int main(int argc, char **argv)
{
  bool available = argc > 1 && strcmp(argv[1], "available") == 0;
  unsetenv("MOONLIGHT_VIDEO_PIPELINE");
  /* This host build has no other video backends. Auto must not load VFM. */
  assert(platform_check("auto") == NONE);
  assert(platform_check("aml-vfm") == (available ? AML_VFM : NONE));
  setenv("MOONLIGHT_VIDEO_PIPELINE", "aml-vfm", 1);
  assert(platform_check("auto") == (available ? AML_VFM : NONE));
  assert(platform_check("aml") == (available ? AML_VFM : NONE));
  assert(platform_check("fake") == FAKE); /* explicit other backend respected */
  if (available) {
    assert(platform_get_video(AML_VFM));
    assert(platform_prefers_codec(AML_VFM, CODEC_HEVC));
    assert(!platform_prefers_codec(AML_VFM, CODEC_AV1));
  }
  return 0;
}
