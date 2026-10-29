/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "xml.h"
#include "errors.h"

#include <expat.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <stdbool.h>

#define STATUS_OK 200

struct xml_query {
  char *memory;
  size_t size;
  int start;
  void* data;
};

static void XMLCALL _xml_start_element(void *userData, const char *name, const char **atts) {
  struct xml_query *search = (struct xml_query*) userData;
  if (strcmp(search->data, name) == 0)
    search->start++;
}

static void XMLCALL _xml_end_element(void *userData, const char *name) {
  struct xml_query *search = (struct xml_query*) userData;
  if (strcmp(search->data, name) == 0)
    search->start--;
}

/* Application parsing has independent state. Do not reuse xml_query's
 * uninitialized one-byte buffer or return partially initialized APP_LISTs. */
struct app_query {
  PAPP_LIST head, current;
  char *text;
  size_t length;
  int active_field; /* 0 = none, 1 = ID, 2 = AppTitle */
  bool id_seen, title_seen;
  int error;
};

void xml_free_applist(PAPP_LIST list) {
  while (list) {
    PAPP_LIST next = list->next;
    free(list->name);
    free(list);
    list = next;
  }
}

static void XMLCALL _app_start(void *ctx, const char *name, const char **atts) {
  struct app_query *q = ctx;
  (void)atts;
  if (q->error) return;
  if (strcmp(name, "App") == 0) {
    if (q->current) { q->error = GS_INVALID; return; }
    PAPP_LIST app = calloc(1, sizeof(*app));
    if (!app) { q->error = GS_OUT_OF_MEMORY; return; }
    app->next = q->head;
    q->head = q->current = app;
    q->id_seen = q->title_seen = false;
  } else if (q->current && (strcmp(name, "ID") == 0 ||
                              strcmp(name, "AppTitle") == 0)) {
    int field = strcmp(name, "ID") == 0 ? 1 : 2;
    if (q->active_field || (field == 1 ? q->id_seen : q->title_seen)) {
      q->error = GS_INVALID;
      return;
    }
    q->text = calloc(1, 1);
    if (!q->text) { q->error = GS_OUT_OF_MEMORY; return; }
    q->length = 0;
    q->active_field = field;
  }
}

static void XMLCALL _app_chars(void *ctx, const XML_Char *chunk, int len) {
  struct app_query *q = ctx;
  if (q->error || !q->active_field || len <= 0) return;
  /* Prevent unbounded allocations from a malformed server response. */
  if ((size_t)len > 4096 - q->length) {
    q->error = GS_INVALID;
    return;
  }
  char *next = realloc(q->text, q->length + (size_t)len + 1);
  if (!next) { q->error = GS_OUT_OF_MEMORY; return; }
  q->text = next;
  memcpy(q->text + q->length, chunk, (size_t)len);
  q->length += (size_t)len;
  q->text[q->length] = '\0';
}

static void XMLCALL _app_end(void *ctx, const char *name) {
  struct app_query *q = ctx;
  if (q->error) return;
  int field = strcmp(name, "ID") == 0 ? 1 :
              strcmp(name, "AppTitle") == 0 ? 2 : 0;
  if (field && q->current && q->active_field == field) {
    if (!q->text || !q->length) {
      q->error = GS_INVALID;
      return;
    }
    if (field == 1) {
      for (size_t i = 0; i < q->length; ++i)
        if (q->text[i] < '0' || q->text[i] > '9') {
          q->error = GS_INVALID;
          return;
        }
      errno = 0;
      char *end;
      long value = strtol(q->text, &end, 10);
      if (errno || !end || *end || value > INT_MAX || value < 0) {
        q->error = GS_INVALID;
        return;
      }
      q->current->id = (int)value;
      q->id_seen = true;
    } else {
      bool visible = false;
      for (size_t i = 0; i < q->length; ++i)
        if (!isspace((unsigned char)q->text[i])) visible = true;
      if (!visible) { q->error = GS_INVALID; return; }
      q->current->name = q->text;
      q->text = NULL;
      q->title_seen = true;
    }
    free(q->text);
    q->text = NULL;
    q->length = 0;
    q->active_field = 0;
  } else if (strcmp(name, "App") == 0 && q->current) {
    if (q->active_field || !q->id_seen || !q->title_seen ||
        !q->current->name) {
      q->error = GS_INVALID;
      return;
    }
    q->current = NULL;
  }
}

static void XMLCALL _xml_start_mode_element(void *userData, const char *name, const char **atts) {
  struct xml_query *search = (struct xml_query*) userData;
  if (strcmp("DisplayMode", name) == 0) {
    PDISPLAY_MODE mode = calloc(1, sizeof(DISPLAY_MODE));
    if (mode != NULL) {
      mode->next = (PDISPLAY_MODE) search->data;
      search->data = mode;
    }
  } else if (search->data != NULL && (strcmp("Height", name) == 0 || strcmp("Width", name) == 0 || strcmp("RefreshRate", name) == 0)) {
    search->memory = malloc(1);
    search->size = 0;
    search->start = 1;
  }
}

static void XMLCALL _xml_end_mode_element(void *userData, const char *name) {
  struct xml_query *search = (struct xml_query*) userData;
  if (search->data != NULL && search->start) {
    PDISPLAY_MODE mode = (PDISPLAY_MODE) search->data;
    if (strcmp("Width", name) == 0)
      mode->width = atoi(search->memory);
    else if (strcmp("Height", name) == 0)
      mode->height = atoi(search->memory);
    else if (strcmp("RefreshRate", name) == 0)
      mode->refresh = atoi(search->memory);

    free(search->memory);
    search->start = 0;
  }
}

static void XMLCALL _xml_start_status_element(void *userData, const char *name, const char **atts) {
  if (strcmp("root", name) == 0) {
    int* status = (int*) userData;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp("status_code", atts[i]) == 0)
        *status = atoi(atts[i + 1]);
      else if (*status != STATUS_OK && strcmp("status_message", atts[i]) == 0)
        gs_error = strdup(atts[i + 1]);
    }
  }
}

static void XMLCALL _xml_end_status_element(void *userData, const char *name) { }

static void XMLCALL _xml_write_data(void *userData, const XML_Char *s, int len) {
  struct xml_query *search = (struct xml_query*) userData;
  if (search->start > 0) {
    search->memory = realloc(search->memory, search->size + len + 1);
    if(search->memory == NULL)
      return;

    memcpy(&(search->memory[search->size]), s, len);
    search->size += len;
    search->memory[search->size] = 0;
  }
}

int xml_search(char* data, size_t len, char* node, char** result) {
  struct xml_query search;
  search.data = node;
  search.start = 0;
  search.memory = calloc(1, 1);
  search.size = 0;
  XML_Parser parser = XML_ParserCreate("UTF-8");
  XML_SetUserData(parser, &search);
  XML_SetElementHandler(parser, _xml_start_element, _xml_end_element);
  XML_SetCharacterDataHandler(parser, _xml_write_data);
  if (! XML_Parse(parser, data, len, 1)) {
    int code = XML_GetErrorCode(parser);
    gs_error = XML_ErrorString(code);
    XML_ParserFree(parser);
    free(search.memory);
    return GS_INVALID;
  } else if (search.memory == NULL) {
    XML_ParserFree(parser);
    return GS_OUT_OF_MEMORY;
  }

  XML_ParserFree(parser);
  *result = search.memory;

  return GS_OK;
}

int xml_applist(char* data, size_t len, PAPP_LIST *app_list) {
  if (!app_list || !data || len > INT_MAX)
    return GS_INVALID;
  *app_list = NULL;
  struct app_query q = {0};
  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) return GS_OUT_OF_MEMORY;
  XML_SetUserData(parser, &q);
  XML_SetElementHandler(parser, _app_start, _app_end);
  XML_SetCharacterDataHandler(parser, _app_chars);
  if (XML_Parse(parser, data, (int)len, XML_TRUE) != XML_STATUS_OK) {
    gs_error = XML_ErrorString(XML_GetErrorCode(parser));
    q.error = GS_INVALID;
  }
  XML_ParserFree(parser);
  if (!q.error && (q.active_field || q.current))
    q.error = GS_INVALID;
  if (q.error) {
    free(q.text);
    xml_free_applist(q.head);
    if (q.error == GS_INVALID && !gs_error)
      gs_error = "Invalid or incomplete application XML";
    return q.error;
  }
  *app_list = q.head;
  return GS_OK;
}

int xml_modelist(char* data, size_t len, PDISPLAY_MODE *mode_list) {
  struct xml_query query = {0};
  query.memory = calloc(1, 1);
  XML_Parser parser = XML_ParserCreate("UTF-8");
  XML_SetUserData(parser, &query);
  XML_SetElementHandler(parser, _xml_start_mode_element, _xml_end_mode_element);
  XML_SetCharacterDataHandler(parser, _xml_write_data);
  if (! XML_Parse(parser, data, len, 1)) {
    int code = XML_GetErrorCode(parser);
    gs_error = XML_ErrorString(code);
    XML_ParserFree(parser);
    return GS_INVALID;
  }

  XML_ParserFree(parser);
  *mode_list = (PDISPLAY_MODE) query.data;

  return GS_OK;

}

int xml_status(char* data, size_t len) {
  int status = 0;
  XML_Parser parser = XML_ParserCreate("UTF-8");
  XML_SetUserData(parser, &status);
  XML_SetElementHandler(parser, _xml_start_status_element, _xml_end_status_element);
  if (!XML_Parse(parser, data, len, 1)) {
    int code = XML_GetErrorCode(parser);
    gs_error = XML_ErrorString(code);
    XML_ParserFree(parser);
    return GS_INVALID;
  }

  XML_ParserFree(parser);
  return status == STATUS_OK ? GS_OK : GS_ERROR;
}
