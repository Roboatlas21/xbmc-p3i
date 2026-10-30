/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../libgamestream/port_validation.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  unsigned short port = 0;
  assert(moonlight_parse_https_port(NULL, &port) && port == 47984);
  assert(moonlight_parse_https_port("", &port) && port == 47984);
  assert(moonlight_parse_https_port("47984", &port) && port == 47984);
  assert(moonlight_parse_https_port("1", &port) && port == 1);
  assert(moonlight_parse_https_port("65535", &port) && port == 65535);
  assert(moonlight_parse_https_port("00443", &port) && port == 443);
  const char *invalid[] = {
      "0", "-1", "+47984", "65536", "65537", "999999999999999999",
      "47984junk", "47984 ", " 47984", "1.2", "\n", "abc", "１２３"
  };
  const unsigned short original = port;
  for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    assert(!moonlight_parse_https_port(invalid[i], &port));
    assert(port == original); /* Never publish a partially parsed port. */
  }
  assert(!moonlight_parse_https_port("47984", NULL));
  puts("PASS: strict server-advertised HTTPS port bounds");
  return 0;
}
