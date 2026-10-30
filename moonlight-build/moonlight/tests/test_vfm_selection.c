/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../src/platform.h"
#include <assert.h>
#include <string.h>

void loop_request_stop(void) {}
void loop_request_error_stop(void) {}
static int writes;
static bool starting;
void write_bool(char *path, bool value) {
  assert(writes < 3);
  bool blank = strstr(path, "/blank") != NULL;
  assert(blank || strcmp(path, "/sys/class/video/disable_video") == 0);
  assert(value == (blank ? starting : !starting));
  ++writes;
}

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
    starting = true;
    platform_start(AML_VFM);
    assert(writes == 3);
    starting = false; writes = 0;
    platform_stop(AML_VFM);
    assert(writes == 3);
  }
  return 0;
}
