/* Inject allocation failure during Expat parser creation. */
#include "../libgamestream/xml.h"
#include "../libgamestream/errors.h"
#include <expat.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

const char *gs_error = NULL;
XML_Parser xml_test_parser_create(const XML_Char *encoding) {
  (void)encoding;
  return NULL;
}
int main(void) {
  const char *xml = "<root status_code='200'/>";
  assert(xml_status((char *)xml, strlen(xml)) == GS_OUT_OF_MEMORY);
  puts("PASS: Sunshine status parser creation failure");
  return 0;
}
