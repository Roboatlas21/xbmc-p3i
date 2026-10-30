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

#include "http.h"
#include "errors.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <curl/curl.h>

static CURL *curl;

static bool debug;
static int http_cancelled;
void http_cancel_pending_requests(void) {
  __atomic_store_n(&http_cancelled, 1, __ATOMIC_RELEASE);
}
void http_reset_pending_cancellation(void) {
  __atomic_store_n(&http_cancelled, 0, __ATOMIC_RELEASE);
}
static int check_http_cancel(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
  (void)userdata; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
  return __atomic_load_n(&http_cancelled, __ATOMIC_ACQUIRE) != 0;
}

/* Shared GameStream response bound, including XML and application listings. */
#define HTTP_MAX_RESPONSE_BYTES (16U * 1024U * 1024U)

/* A paired Sunshine host signs its public certificate during the PIN
 * handshake. Reuse that verified public key for subsequent HTTPS requests.
 * Existing clients without a stored PIN keep legacy behavior until re-paired.
 * Never silently pin the first certificate observed over unauthenticated HTTPS. */
static char server_cert_path[PATH_MAX];
static char server_public_key_pin[64];
static char pending_server_cert[8193];
static char committed_public_key_pin[64];

static int public_key_pin_from_pem(const char *pem, char output[64]) {
  BIO *input = NULL;
  X509 *certificate = NULL;
  EVP_PKEY *key = NULL;
  unsigned char *encoded = NULL;
  int rc = GS_FAILED;
  if (!pem || strlen(pem) > 8192) return GS_INVALID;
  input = BIO_new_mem_buf(pem, -1);
  if (!input) goto done;
  certificate = PEM_read_bio_X509(input, NULL, NULL, NULL);
  if (!certificate) goto done;
  key = X509_get_pubkey(certificate);
  if (!key) goto done;
  int length = i2d_PUBKEY(key, NULL);
  if (length < 1 || length > 8192) goto done;
  encoded = OPENSSL_malloc(length);
  if (!encoded) { rc = GS_OUT_OF_MEMORY; goto done; }
  unsigned char *cursor = encoded;
  if (i2d_PUBKEY(key, &cursor) != length) goto done;
  unsigned char digest[SHA256_DIGEST_LENGTH];
  unsigned char b64[64];
  if (!SHA256(encoded, (size_t)length, digest)) goto done;
  if (EVP_EncodeBlock(b64, digest, sizeof(digest)) != 44) goto done;
  /* SHA-256 encodes to exactly 44 base64 characters. Avoid truncation
   * warnings and do not rely on an unchecked printf string width. */
  memcpy(output, "sha256//", 8);
  memcpy(output + 8, b64, 44);
  output[52] = '\0';
  rc = GS_OK;
done:
  OPENSSL_free(encoded);
  EVP_PKEY_free(key);
  X509_free(certificate);
  BIO_free(input);
  return rc;
}

/* Host identifiers select exactly one local pin file, never a path supplied
 * by a remote host. Forgetting a pin does not touch client.pem/key.pem. */
static int server_certificate_path(const char *keyDirectory, const char *address,
                                   char *path, size_t capacity) {
  if (!keyDirectory || !address || !*address || !path || !capacity)
    return GS_INVALID;
  unsigned char digest[SHA256_DIGEST_LENGTH];
  char hex[65];
  SHA256((const unsigned char *)address, strlen(address), digest);
  for (unsigned int i = 0; i < sizeof(digest); ++i)
    snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02x", digest[i]);
  return snprintf(path, capacity, "%s/server-%s.pem",
                  keyDirectory, hex) >= (int)capacity ? GS_INVALID : GS_OK;
}

int http_forget_server_certificate(const char *keyDirectory, const char *address) {
  char path[PATH_MAX];
  if (server_certificate_path(keyDirectory, address, path, sizeof(path)) != GS_OK) {
    gs_error = "Invalid trusted-server certificate path";
    return GS_INVALID;
  }
  if (unlink(path) != 0 && errno != ENOENT) {
    gs_error = "Unable to delete this host's trusted certificate";
    return GS_FAILED;
  }
  /* Reset only if this is the identity currently bound in-process. */
  if (strcmp(path, server_cert_path) == 0) {
    server_public_key_pin[0] = '\0';
    committed_public_key_pin[0] = '\0';
    pending_server_cert[0] = '\0';
  }
  return GS_OK;
}

int http_bind_server_certificate(const char *keyDirectory, const char *address) {
  server_cert_path[0] = '\0';
  server_public_key_pin[0] = '\0';
  committed_public_key_pin[0] = '\0';
  pending_server_cert[0] = '\0';
  if (server_certificate_path(keyDirectory, address, server_cert_path,
                              sizeof(server_cert_path)) != GS_OK) {
    gs_error = "Pinned server certificate path is too long";
    return GS_INVALID;
  }
  FILE *f = fopen(server_cert_path, "rb");
  if (!f) {
    if (errno == ENOENT) return GS_OK;
    gs_error = "Cannot read pinned Sunshine certificate";
    return GS_FAILED;
  }
  char pem[8193] = {0};
  size_t count = fread(pem, 1, sizeof(pem) - 1, f);
  int bad = ferror(f) || count == sizeof(pem) - 1;
  if (fclose(f) != 0) bad = 1;
  if (bad || public_key_pin_from_pem(pem, committed_public_key_pin) != GS_OK) {
    gs_error = "Pinned Sunshine certificate is unreadable or malformed";
    return GS_FAILED;
  }
  memcpy(server_public_key_pin, committed_public_key_pin,
         strlen(committed_public_key_pin) + 1);
  return GS_OK;
}

int http_prepare_paired_certificate(const char *pem) {
  char expected[64] = {0};
  int rc = public_key_pin_from_pem(pem, expected);
  if (rc != GS_OK) { gs_error = "Invalid certificate from pairing handshake"; return rc; }
  if (committed_public_key_pin[0] &&
      strcmp(committed_public_key_pin, expected) != 0) {
    gs_error = "Sunshine certificate changed: remove old host pin only after verifying host identity";
    return GS_FAILED;
  }
  memcpy(pending_server_cert, pem, strlen(pem) + 1);
  memcpy(server_public_key_pin, expected, strlen(expected) + 1);
  return GS_OK;
}

void http_discard_pending_certificate(void) {
  pending_server_cert[0] = '\0';
  memcpy(server_public_key_pin, committed_public_key_pin,
         strlen(committed_public_key_pin) + 1);
}

int http_commit_paired_certificate(void) {
  if (!pending_server_cert[0] || !server_cert_path[0]) return GS_INVALID;
  if (committed_public_key_pin[0]) {
    http_discard_pending_certificate();
    return GS_OK;
  }
  char temporary[PATH_MAX];
  int path_length = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", server_cert_path);
  if (path_length < 0 || (size_t)path_length >= sizeof(temporary)) {
    gs_error = "Trusted host certificate path is too long";
    return GS_INVALID;
  }
  int fd = mkstemp(temporary);
  if (fd < 0) {
    gs_error = "Unable to create the trusted host certificate file; check the pairing directory permissions";
    return GS_FAILED;
  }
  fchmod(fd, 0600);
  size_t length = strlen(pending_server_cert);
  int rc = GS_FAILED;
  size_t done = 0;
  while (done < length) {
    ssize_t wrote = write(fd, pending_server_cert + done, length - done);
    if (wrote < 0 && errno == EINTR) continue;
    if (wrote <= 0) break;
    done += (size_t)wrote;
  }
  int synced = done == length && fsync(fd) == 0;
  int closed = close(fd) == 0;
  fd = -1;
  if (synced && closed && rename(temporary, server_cert_path) == 0)
    rc = GS_OK;
  if (rc != GS_OK) {
    unlink(temporary);
    gs_error = "Unable to persist verified Sunshine host certificate";
    return rc;
  }
  memcpy(committed_public_key_pin, server_public_key_pin,
         strlen(server_public_key_pin) + 1);
  pending_server_cert[0] = '\0';
  return GS_OK;
}

bool http_server_certificate_pinned(void) {
  return server_public_key_pin[0] != '\0';
}


static size_t _write_curl(void *contents, size_t size, size_t nmemb, void *userp)
{
  PHTTP_DATA mem = (PHTTP_DATA)userp;
  if (size && nmemb > SIZE_MAX / size)
    return 0;
  const size_t realsize = size * nmemb;
  if (mem->size > HTTP_MAX_RESPONSE_BYTES ||
      realsize > HTTP_MAX_RESPONSE_BYTES - mem->size)
    return 0;

  /* Keep the prior allocation intact if realloc fails. */
  char *resized = realloc(mem->memory, mem->size + realsize + 1);
  if (!resized) return 0;
  mem->memory = resized;

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

int http_init(const char* keyDirectory, int logLevel) {
  curl = curl_easy_init();
  debug = logLevel >= 2;
  if (!curl)
    return GS_FAILED;

  char certificateFilePath[4096];
  snprintf(certificateFilePath, sizeof(certificateFilePath), "%s/%s", keyDirectory, CERTIFICATE_FILE_NAME);

  char keyFilePath[4096];
  snprintf(keyFilePath, sizeof(keyFilePath), "%s/%s", keyDirectory, KEY_FILE_NAME);

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(curl, CURLOPT_SSLENGINE_DEFAULT, 1L);
  curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE,"PEM");
  curl_easy_setopt(curl, CURLOPT_SSLCERT, certificateFilePath);
  curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
  curl_easy_setopt(curl, CURLOPT_SSLKEY, keyFilePath);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_curl);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  if (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, check_http_cancel)
      != CURLE_OK) {
    gs_error = "This libcurl cannot cancel interrupted GameStream requests";
    curl_easy_cleanup(curl);
    curl = NULL;
    return GS_FAILED;
  }
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_SESSIONID_CACHE, 0L);
  /* Sunshine can accept TCP on its HTTPS listener yet never complete the
   * request. Never strand Moonlight indefinitely in curl_easy_perform(). */
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);

  return GS_OK;
}

int http_request(char* url, PHTTP_DATA data) {
  return http_request_with_timeout(url, data, 15000L);
}

int http_request_with_timeout(char* url, PHTTP_DATA data, long timeout_ms) {
  if (__atomic_load_n(&http_cancelled, __ATOMIC_ACQUIRE)) {
    gs_error = "GameStream HTTP request interrupted during startup";
    return GS_FAILED;
  }
  /* Set this for every request: the shared handle must not retain a pairing
   * timeout for later status, launch, or handshake requests. */
  /* Public HTTP pairing/bootstrap endpoints remain unpinned. Authenticated
   * HTTPS calls enforce the key validated during the PIN handshake. */
#if LIBCURL_VERSION_NUM >= 0x072700
  const char *pin = strncmp(url, "https://", 8) == 0 &&
       server_public_key_pin[0] ? server_public_key_pin : NULL;
  CURLcode pin_result = curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, pin);
  /* Do not break legacy unpinned HTTP hosts on an older TLS backend.
   * When a pin EXISTS, however, failure to enforce it must stop the call. */
  if (pin && pin_result != CURLE_OK) {
    gs_error = "TLS backend cannot enforce the Sunshine public-key pin";
    return GS_FAILED;
  }
#else
  if (strncmp(url, "https://", 8) == 0 && server_public_key_pin[0]) {
    gs_error = "This libcurl cannot validate pinned Sunshine keys";
    return GS_FAILED;
  }
#endif
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
  curl_easy_setopt(curl, CURLOPT_URL, url);
#ifdef __FreeBSD__
  curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1);
#endif

  if (debug) {
    /* Pairing and launch query strings contain PIN challenge material,
     * client certificates, signed secrets, and remote-input AES keys.
     * Log only the endpoint, never any query parameters. */
    const char *query = strchr(url, '?');
    const size_t length = query ? (size_t)(query - url) : strlen(url);
    printf("Request %.*s%s\n", (int)length, url,
           query ? "?[query redacted]" : "");
    fflush(stdout);
  }

  if (data->size > 0) {
    free(data->memory);
    data->memory = malloc(1);
    if(data->memory == NULL) {
      data->size = 0;
      return GS_OUT_OF_MEMORY;
    }
    data->size = 0;
  }
  if (data->memory)
    data->memory[0] = '\0';
  CURLcode res = curl_easy_perform(curl);

  if(res != CURLE_OK) {
    static char request_error[256];
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    snprintf(request_error, sizeof(request_error), "%s (curl %d, HTTP %ld)",
             curl_easy_strerror(res), (int)res, status);
    gs_error = request_error;
    if (debug) {
      fprintf(stderr, "HTTP request failed: %s\n", gs_error);
    }
    return GS_FAILED;
  } else if (data->memory == NULL) {
    return GS_OUT_OF_MEMORY;
  }

  if (debug) {
    /* XML bodies can also contain pairing secrets and server certificates. */
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    printf("Response: HTTP %ld, %zu bytes (body redacted)\n",
           status, data->size);
    fflush(stdout);
  }

  return GS_OK;
}

void http_cleanup() {
  curl_easy_cleanup(curl);
}

PHTTP_DATA http_create_data() {
  PHTTP_DATA data = malloc(sizeof(HTTP_DATA));
  if (data == NULL)
    return NULL;

  data->memory = malloc(1);
  if(data->memory == NULL) {
    free(data);
    return NULL;
  }
  data->size = 0;
  data->memory[0] = '\0';
  return data;
}

void http_free_data(PHTTP_DATA data) {
  if (data != NULL) {
    if (data->memory != NULL)
      free(data->memory);

    free(data);
  }
}
