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

static void reject_mode(const char *xml) {
  PDISPLAY_MODE modes = (PDISPLAY_MODE)1;
  assert(xml_modelist((char *)xml, strlen(xml), &modes) != GS_OK);
  assert(modes == NULL);
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

  PDISPLAY_MODE modes = NULL;
  const char *mode_xml =
      "<root><DisplayMode><Width>1920</Width><Height>1080</Height>"
      "<RefreshRate>60</RefreshRate></DisplayMode>"
      "<DisplayMode><Width>1280</Width><Height>720</Height>"
      "<RefreshRate>120</RefreshRate></DisplayMode></root>";
  assert(xml_modelist((char *)mode_xml, strlen(mode_xml), &modes) == GS_OK);
  assert(modes && modes->width == 1280 && modes->height == 720 && modes->refresh == 120);
  assert(modes->next && modes->next->width == 1920 && modes->next->refresh == 60);
  free(modes->next);
  free(modes);
  modes = (PDISPLAY_MODE)1;
  assert(xml_modelist("<root/>", strlen("<root/>"), &modes) == GS_OK);
  assert(modes == NULL);

  reject_mode("<root><DisplayMode><Width/><Height>1080</Height><RefreshRate>60</RefreshRate></DisplayMode></root>");
  reject_mode("<root><DisplayMode><Width>bad</Width><Height>1080</Height><RefreshRate>60</RefreshRate></DisplayMode></root>");
  reject_mode("<root><DisplayMode><Width>999999999999999999999</Width><Height>1080</Height><RefreshRate>60</RefreshRate></DisplayMode></root>");
  reject_mode("<root><DisplayMode><Width>1920</Width><Height>1080</Height></DisplayMode></root>");
  reject_mode("<root><DisplayMode><Width>1920</Width><Width>100</Width><Height>1080</Height><RefreshRate>60</RefreshRate></DisplayMode></root>");
  reject_mode("<root><DisplayMode><Width>1920</Width><Height>1080</Height>");
  reject_mode("<root><DisplayMode><Width>1920</Width><Height>1080</Height><RefreshRate>60</RefreshRate><DisplayMode/></DisplayMode></root>");

  char *field = NULL;
  const char *field_xml = "<root><name>first<![CDATA[second]]>third</name></root>";
  assert(xml_search((char *)field_xml, strlen(field_xml), "name", &field) == GS_OK);
  assert(field && strcmp(field, "firstsecondthird") == 0);
  free(field);
  field = (char *)1;
  assert(xml_search(NULL, 0, "name", &field) == GS_INVALID);
  assert(field == (char *)1); /* Bad API call does not dereference inputs. */
  const char *ok_status = "<root status_message='OK' status_code='200'/>";
  assert(xml_status((char *)ok_status, strlen(ok_status)) == GS_OK);
  const char *bad_status = "<root status_code='400' status_message='Bad request'/>";
  assert(xml_status((char *)bad_status, strlen(bad_status)) == GS_ERROR);
  assert(gs_error && strstr(gs_error, "Bad request") != NULL);
  /* Error messages use a bounded owned buffer; callers must not free gs_error. */
  gs_error = NULL;
  const char *invalid_statuses[] = {
    "<root status_code='200garbage'/>",
    "<root status_code=' 200'/>",
    "<root status_code='200 ' />",
    "<root status_code='-200'/>",
    "<root status_code=''/>",
    "<root status_code='99999999999999999'/>",
    "<root/>",
    "<elsewhere status_code='200'/>",
    "<root status_code='200' status_code='200'/>",
    "<root status_code='200'>",
  };
  for (size_t i = 0; i < sizeof(invalid_statuses) / sizeof(invalid_statuses[0]); ++i) {
    const char *xml = invalid_statuses[i];
    assert(xml_status((char *)xml, strlen(xml)) == GS_INVALID);
  }
  assert(xml_status(NULL, 0) == GS_INVALID);
  puts("PASS: native XML field, application, display-mode and status parsers");
  return 0;
}
