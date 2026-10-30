/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/config_numeric.h"
#include <assert.h>
#include <stdint.h>

int main(void) {
  int parsed = -999;
  assert(moonlight_parse_bounded_int("3840", 64, 7680, &parsed));
  assert(parsed == 3840);
  assert(moonlight_parse_bounded_int("-1", -1, 500000, &parsed));
  assert(parsed == -1);
  assert(!moonlight_parse_bounded_int(NULL, 1, 240, &parsed));
  assert(!moonlight_parse_bounded_int("", 1, 240, &parsed));
  assert(!moonlight_parse_bounded_int("60fps", 1, 240, &parsed));
  assert(!moonlight_parse_bounded_int("12.5", 1, 240, &parsed));
  assert(!moonlight_parse_bounded_int("0", 1, 240, &parsed));
  assert(!moonlight_parse_bounded_int("-60", 1, 240, &parsed));
  assert(!moonlight_parse_bounded_int("999999999999999999999", 1, 240, &parsed));
  assert(!moonlight_parse_bounded_int("50000", 64, 7680, &parsed));
  assert(moonlight_parse_bounded_int("120", 1, 240, &parsed));
  assert(parsed == 120);
  assert(moonlight_stream_numbers_valid(3840, 2160, 120, 1392, 100000));
  assert(moonlight_stream_numbers_valid(7680, 4320, 240, 1500, -1));
  assert(!moonlight_stream_numbers_valid(50000, 50000, 60, 1392, -1));
  assert(!moonlight_stream_numbers_valid(3840, 2160, 0, 1392, -1));
  assert(!moonlight_stream_numbers_valid(3840, 2160, 60, 0, -1));
  assert(!moonlight_stream_numbers_valid(3840, 2160, 60, 1392, 0));
  assert(!moonlight_stream_numbers_valid(3840, 2160, 60, 1392, 500001));
  /* Options not covered by the resolution/bitrate model must also use
   * checked parsing: rotation, explicit pairing PIN, and host port. */
  assert(moonlight_parse_bounded_int("270", 0, 270, &parsed));
  assert(!moonlight_parse_bounded_int("271", 0, 270, &parsed));
  assert(!moonlight_parse_bounded_int("270junk", 0, 270, &parsed));
  assert(moonlight_parse_bounded_int("9999", 1, 9999, &parsed));
  assert(!moonlight_parse_bounded_int("0", 1, 9999, &parsed));
  assert(!moonlight_parse_bounded_int("00000", 1, 9999, &parsed));
  assert(moonlight_parse_bounded_int("65535", 1, 65535, &parsed));
  assert(!moonlight_parse_bounded_int("65536", 1, 65535, &parsed));
  assert(!moonlight_parse_bounded_int("notaport", 1, 65535, &parsed));
  assert(!moonlight_parse_bounded_int("-1", 1, 65535, &parsed));
  assert(moonlight_stream_pixel_count(7680, 4320) == UINT64_C(33177600));
  assert(moonlight_stream_pixel_count(50000, 50000) == UINT64_C(2500000000));
  return 0;
}
