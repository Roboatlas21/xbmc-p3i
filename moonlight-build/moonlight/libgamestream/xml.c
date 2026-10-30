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

#define XML_SEARCH_FIELD_LIMIT (2U * 1024U * 1024U)

struct xml_query {
  char *memory;
  size_t size;
  int start;
  int error;
  const char *data;
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

/* DisplayMode is parsed independently of xml_search. Empty fields,
 * repeated fields, oversized numbers, and truncated elements must never
 * reach atoi() or leave partially initialized mode lists. */
struct mode_query {
  PDISPLAY_MODE head, current;
  unsigned int seen;
  unsigned int active_field; /* Width=1, Height=2, RefreshRate=4. */
  char text[32];
  size_t length;
  int error;
};

static void free_mode_list(PDISPLAY_MODE mode) {
  while (mode) {
    PDISPLAY_MODE next = mode->next;
    free(mode);
    mode = next;
  }
}

static unsigned int mode_field(const char *name) {
  if (strcmp(name, "Width") == 0) return 1;
  if (strcmp(name, "Height") == 0) return 2;
  if (strcmp(name, "RefreshRate") == 0) return 4;
  return 0;
}

static void XMLCALL _xml_start_mode_element(void *ctx, const char *name, const char **atts) {
  struct mode_query *q = ctx;
  (void)atts;
  if (q->error) return;
  if (strcmp(name, "DisplayMode") == 0) {
    if (q->current) { q->error = GS_INVALID; return; }
    PDISPLAY_MODE mode = calloc(1, sizeof(*mode));
    if (!mode) { q->error = GS_OUT_OF_MEMORY; return; }
    mode->next = q->head;
    q->head = q->current = mode;
    q->seen = 0;
    q->active_field = 0;
    return;
  }
  const unsigned int field = mode_field(name);
  if (q->current && field) {
    if (q->active_field || (q->seen & field)) {
      q->error = GS_INVALID;
      return;
    }
    q->active_field = field;
    q->length = 0;
    q->text[0] = '\0';
  }
}

static void XMLCALL _xml_mode_chars(void *ctx, const XML_Char *chunk, int len) {
  struct mode_query *q = ctx;
  if (q->error || !q->active_field || len <= 0) return;
  if ((size_t)len >= sizeof(q->text) - q->length) {
    q->error = GS_INVALID;
    return;
  }
  memcpy(q->text + q->length, chunk, (size_t)len);
  q->length += (size_t)len;
  q->text[q->length] = '\0';
}

static void XMLCALL _xml_end_mode_element(void *ctx, const char *name) {
  struct mode_query *q = ctx;
  if (q->error) return;
  const unsigned int field = mode_field(name);
  if (q->current && field && q->active_field == field) {
    char *text = q->text;
    while (isspace((unsigned char)*text)) ++text;
    if (!isdigit((unsigned char)*text)) { q->error = GS_INVALID; return; }
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    while (end && isspace((unsigned char)*end)) ++end;
    if (errno || !end || *end || value == 0 || value > UINT_MAX) {
      q->error = GS_INVALID;
      return;
    }
    if (field == 1) q->current->width = (unsigned int)value;
    if (field == 2) q->current->height = (unsigned int)value;
    if (field == 4) q->current->refresh = (unsigned int)value;
    q->seen |= field;
    q->active_field = 0;
  } else if (strcmp(name, "DisplayMode") == 0) {
    if (!q->current || q->active_field || q->seen != 7) {
      q->error = GS_INVALID;
      return;
    }
    q->current = NULL;
  }
}

struct status_query {
  int code;
  bool root_seen, code_seen;
  int error;
  char message[4097];
  bool message_seen;
};

static void XMLCALL _xml_start_status_element(void *ctx, const char *name,
                                               const char **atts) {
  struct status_query *q = ctx;
  if (q->error || strcmp("root", name) != 0)
    return;
  if (q->root_seen) {
    q->error = GS_INVALID;
    return;
  }
  q->root_seen = true;
  for (int i = 0; atts[i]; i += 2) {
    const char *key = atts[i], *value = atts[i + 1];
    if (strcmp("status_code", key) == 0) {
      if (q->code_seen || !*value) { q->error = GS_INVALID; return; }
      int code = 0;
      /* Sunshine responses use decimal HTTP-like status codes. atoi()
       * accepted a malformed value such as "200invalid" as success. */
      for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p < '0' || *p > '9' || code > (999 - (*p - '0')) / 10) {
          q->error = GS_INVALID;
          return;
        }
        code = code * 10 + (*p - '0');
      }
      if (code == 0) { q->error = GS_INVALID; return; }
      q->code_seen = true;
      q->code = code;
    } else if (strcmp("status_message", key) == 0) {
      const size_t length = strlen(value);
      if (q->message_seen || length >= sizeof(q->message)) {
        q->error = GS_INVALID;
        return;
      }
      memcpy(q->message, value, length + 1);
      q->message_seen = true;
    }
  }
}

static void XMLCALL _xml_end_status_element(void *ctx, const char *name) {
  (void)ctx; (void)name;
}

static void XMLCALL _xml_write_data(void *userData, const XML_Char *chunk, int len) {
  struct xml_query *search = (struct xml_query*)userData;
  if (!search->start || search->error || len <= 0)
    return;
  if (search->size > XML_SEARCH_FIELD_LIMIT ||
      (size_t)len > XML_SEARCH_FIELD_LIMIT - search->size) {
    search->error = GS_INVALID;
    return;
  }
  const size_t needed = search->size + (size_t)len + 1;
  char *updated = realloc(search->memory, needed);
  if (!updated) {
    search->error = GS_OUT_OF_MEMORY;
    return; /* Keep original allocation for cleanup. */
  }
  search->memory = updated;
  memcpy(search->memory + search->size, chunk, (size_t)len);
  search->size += (size_t)len;
  search->memory[search->size] = '\0';
}

int xml_search(char* data, size_t len, char* node, char** result) {
  if (!result || !data || !node || !*node || len > INT_MAX)
    return GS_INVALID;
  *result = NULL;
  struct xml_query search = { .data = node };
  search.memory = calloc(1, 1);
  if (!search.memory)
    return GS_OUT_OF_MEMORY;
  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) {
    free(search.memory);
    return GS_OUT_OF_MEMORY;
  }
  XML_SetUserData(parser, &search);
  XML_SetElementHandler(parser, _xml_start_element, _xml_end_element);
  XML_SetCharacterDataHandler(parser, _xml_write_data);
  int parse_status = XML_Parse(parser, data, (int)len, XML_TRUE);
  int result_code = search.error;
  if (!result_code && parse_status != XML_STATUS_OK) {
    gs_error = XML_ErrorString(XML_GetErrorCode(parser));
    result_code = GS_INVALID;
  }
  XML_ParserFree(parser);
  if (result_code) {
    free(search.memory);
    return result_code;
  }
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
  if (!mode_list || !data || len > INT_MAX) return GS_INVALID;
  *mode_list = NULL;
  struct mode_query q = {0};
  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) return GS_OUT_OF_MEMORY;
  XML_SetUserData(parser, &q);
  XML_SetElementHandler(parser, _xml_start_mode_element, _xml_end_mode_element);
  XML_SetCharacterDataHandler(parser, _xml_mode_chars);
  if (XML_Parse(parser, data, (int)len, XML_TRUE) != XML_STATUS_OK) {
    gs_error = XML_ErrorString(XML_GetErrorCode(parser));
    q.error = GS_INVALID;
  }
  XML_ParserFree(parser);
  if (!q.error && (q.current || q.active_field)) q.error = GS_INVALID;
  if (q.error) {
    free_mode_list(q.head);
    if (q.error == GS_INVALID && !gs_error)
      gs_error = "Invalid or incomplete display mode XML";
    return q.error;
  }
  *mode_list = q.head;
  return GS_OK;
}

int xml_status(char* data, size_t len) {
  if (!data || len > INT_MAX)
    return GS_INVALID;
  struct status_query q = {0};
  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser)
    return GS_OUT_OF_MEMORY;
  XML_SetUserData(parser, &q);
  XML_SetElementHandler(parser, _xml_start_status_element, _xml_end_status_element);
  int parsed = XML_Parse(parser, data, (int)len, XML_TRUE);
  if (!q.error && parsed != XML_STATUS_OK) {
    gs_error = XML_ErrorString(XML_GetErrorCode(parser));
    q.error = GS_INVALID;
  }
  XML_ParserFree(parser);
  if (q.error || !q.root_seen || !q.code_seen) {
    const int error = q.error ? q.error : GS_INVALID;
    if (error == GS_INVALID)
      gs_error = "Missing or malformed Sunshine status";
    return error;
  }
  if (q.code == STATUS_OK)
    return GS_OK;
  /* gs_error may point to literals elsewhere. Keep this response text in
   * bounded owned storage rather than leaking a strdup on each failure. */
  static char last_status_error[4097];
  if (q.message_seen) {
    memcpy(last_status_error, q.message, strlen(q.message) + 1);
    gs_error = last_status_error;
  } else {
    gs_error = "Sunshine returned an error status";
  }
  return GS_ERROR;
}
