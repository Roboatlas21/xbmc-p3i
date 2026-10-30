/* Fault-inject the second XML character-buffer resize. */
#include "../libgamestream/xml.h"
#include "../libgamestream/errors.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static int calls, fail_at;
const char *gs_error = NULL;
void *xml_test_realloc(void *ptr, size_t size) {
  if (++calls == fail_at) return NULL;
  return realloc(ptr, size);
}
int main(void) {
  const char *source = "<root><name>A<![CDATA[B]]>C</name></root>";
  char *value = NULL;
  fail_at = 2;
  assert(xml_search((char *)source, strlen(source), "name", &value) == GS_OUT_OF_MEMORY);
  assert(value == NULL);
  calls = 0; fail_at = 0;
  assert(xml_search((char *)source, strlen(source), "name", &value) == GS_OK);
  assert(value && strcmp(value, "ABC") == 0);
  free(value);
  puts("PASS: XML realloc failure is not reported as success");
  return 0;
}
