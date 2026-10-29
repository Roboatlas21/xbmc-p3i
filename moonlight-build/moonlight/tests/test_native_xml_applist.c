/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../libgamestream/xml.h"
#include "../libgamestream/errors.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

const char *gs_error = NULL;

static void rejects(const char *xml) {
  PAPP_LIST list = (PAPP_LIST)1;
  int status = xml_applist((char *)xml, strlen(xml), &list);
  assert(status != GS_OK);
  assert(list == NULL); /* Failed partial lists are always reclaimed. */
}

int main(void) {
  const char *valid =
      "<root status_code='200'><App><ID>42</ID>"
      "<AppTitle>Ratchet &amp; Clank</AppTitle></App>"
      "<App><ID>23</ID><AppTitle>Pok&#233;mon</AppTitle></App></root>";
  PAPP_LIST list = NULL;
  assert(xml_applist((char *)valid, strlen(valid), &list) == GS_OK);
  assert(list && list->name);
  assert(strcmp(list->name, "Pok\303\251mon") == 0 && list->id == 23);
  assert(list->next && list->next->id == 42);
  assert(strcmp(list->next->name, "Ratchet & Clank") == 0);
  xml_free_applist(list);

  const char *empty = "<root status_code='200'/>";
  assert(xml_applist((char *)empty, strlen(empty), &list) == GS_OK);
  assert(list == NULL);

  rejects("<root><App><ID>5</ID><AppTitle/></App></root>");
  rejects("<root><App><ID>5</ID></App></root>");
  rejects("<root><App><AppTitle>Untitled</AppTitle></App></root>");
  rejects("<root><App><ID>abc</ID><AppTitle>X</AppTitle></App></root>");
  rejects("<root><App><ID>999999999999999</ID><AppTitle>X</AppTitle></App></root>");
  rejects("<root><App><ID>1</ID><AppTitle> </AppTitle></App></root>");
  rejects("<root><App><ID>3</ID><AppTitle>X</AppTitle><ID>3</ID></App></root>");
  rejects("<root><App><ID>3</ID><AppTitle>Broken");

  puts("PASS: native application parser rejects incomplete XML safely");
  return 0;
}
