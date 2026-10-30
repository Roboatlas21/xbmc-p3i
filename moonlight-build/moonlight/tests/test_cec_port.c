#include "../src/input/cec_port.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  char source[1024], output[50];
  memset(source, 'X', sizeof(source));
  memcpy(source, "/dev/aocec", 11);
  assert(cec_copy_port_name(output, sizeof(output), source, sizeof(source)));
  assert(strcmp(output, "/dev/aocec") == 0);
  memset(source, 'a', sizeof(source));
  source[49] = '\0';
  assert(cec_copy_port_name(output, sizeof(output), source, sizeof(source)));
  source[49] = 'a';
  source[50] = '\0';
  assert(!cec_copy_port_name(output, sizeof(output), source, sizeof(source)));
  memset(source, 'b', sizeof(source));
  assert(!cec_copy_port_name(output, sizeof(output), source, sizeof(source)));
  source[0] = '\0';
  assert(!cec_copy_port_name(output, sizeof(output), source, sizeof(source)));
  puts("PASS: CEC adapter port boundaries");
  return 0;
}
