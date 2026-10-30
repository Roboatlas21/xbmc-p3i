/* Compile the actual native mapping parser and test invalid env overrides. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/input/mapping.h"

int main(void) {
  static const char text[] = "03000000000000000000000000000000,Example Pad,a:b0,b:b1";
  char original[sizeof(text)];
  memcpy(original, text, sizeof(text));
  struct mapping *valid = mapping_parse_override(text);
  assert(valid != NULL);
  assert(!strcmp(valid->guid, "03000000000000000000000000000000"));
  assert(!strcmp(valid->name, "Example Pad"));
  assert(valid->btn_a == 0 && valid->btn_b == 1);
  assert(!memcmp(text, original, sizeof(text))); /* read-only source unchanged */
  free(valid);
  assert(mapping_parse_override("") == NULL);
  assert(mapping_parse_override("invalid") == NULL);
  assert(mapping_parse_override(NULL) == NULL);

  /* The file loader should not leak descriptors over repeated calls. */
  char filename[] = "/tmp/moonlight-mapping-XXXXXX";
  int fd = mkstemp(filename);
  assert(fd >= 0);
  FILE *f = fdopen(fd, "w");
  assert(f != NULL);
  assert(fprintf(f, "%s\n", text) > 0);
  assert(fclose(f) == 0);
  for (int i = 0; i < 64; ++i) {
    struct mapping *item = mapping_load(filename, false);
    assert(item);
    free(item);
  }
  unlink(filename);
  puts("PASS: immutable mapping override, null mapping, repeated file parse");
  return 0;
}
