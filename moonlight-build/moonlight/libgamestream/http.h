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

#pragma once

#include <stdlib.h>
#include <stdbool.h>

#define CERTIFICATE_FILE_NAME "client.pem"
#define KEY_FILE_NAME "key.pem"

typedef struct _HTTP_DATA {
  char *memory;
  size_t size;
} HTTP_DATA, *PHTTP_DATA;

void http_cancel_pending_requests(void);
void http_reset_pending_cancellation(void);
int http_init(const char* keyDirectory, int logLevel);
/* Pins are stored per host under the same credentials directory as client.pem. */
int http_bind_server_certificate(const char *keyDirectory, const char *address);
int http_forget_server_certificate(const char *keyDirectory, const char *address);
int http_prepare_paired_certificate(const char *pem);
int http_commit_paired_certificate(void);
void http_discard_pending_certificate(void);
bool http_server_certificate_pinned(void);
PHTTP_DATA http_create_data();
int http_request(char* url, PHTTP_DATA data);
int http_request_with_timeout(char* url, PHTTP_DATA data, long timeout_ms);
void http_free_data(PHTTP_DATA data);
