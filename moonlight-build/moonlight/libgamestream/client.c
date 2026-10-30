/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
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
#include "xml.h"
#include "mkcert.h"
#include "client.h"
#include "errors.h"
#include "port_validation.h"
#include "limits.h"

#include <Limelight.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <uuid/uuid.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#define UNIQUE_FILE_NAME "uniqueid.dat"
#define P12_FILE_NAME "client.p12"

#define UNIQUEID_BYTES 8
#define UNIQUEID_CHARS (UNIQUEID_BYTES*2)

static char unique_id[UNIQUEID_CHARS+1];
static X509 *cert;
static char cert_hex[8192];
static EVP_PKEY *privateKey;

const char* gs_error;

#define LEN_AS_HEX_STR(x) ((x) * 2 + 1)
#define SIZEOF_AS_HEX_STR(x) LEN_AS_HEX_STR(sizeof(x))

#define UUID_STRLEN 37

static int mkdirtree(const char* directory) {
  char buffer[PATH_MAX];
  char* p = buffer;

  // The passed in string could be a string literal
  // so we must copy it first
  strncpy(p, directory, PATH_MAX - 1);
  buffer[PATH_MAX - 1] = '\0';

  while (*p != 0) {
    // Find the end of the path element
    do {
      p++;
    } while (*p != 0 && *p != '/');

    char oldChar = *p;
    *p = 0;

    // Create the directory if it doesn't exist already
    if (mkdir(buffer, 0775) == -1 && errno != EEXIST) {
        return -1;
    }

    *p = oldChar;
  }

  return 0;
}

/* Never rewrite an existing device ID after a transient short read. A
 * different ID breaks pairing with every previously registered host. */
static int read_existing_unique_id(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid()) {
    close(fd);
    gs_error = "Existing Moonlight device ID is not an owned regular file";
    return GS_FAILED;
  }
  char id[UNIQUEID_CHARS + 1];
  size_t count = 0;
  while (count < UNIQUEID_CHARS) {
    ssize_t n = read(fd, id + count, UNIQUEID_CHARS - count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    count += (size_t)n;
  }
  int closed = close(fd);
  if (closed != 0 || count != UNIQUEID_CHARS) {
    gs_error = "Existing device ID is truncated or unreadable";
    return GS_FAILED;
  }
  for (size_t i = 0; i < UNIQUEID_CHARS; ++i) {
    char c = id[i];
    if (!((c >= '0' && c <= '9') ||
          (c >= 'A' && c <= 'F') ||
          (c >= 'a' && c <= 'f'))) {
      gs_error = "Existing device ID contains invalid characters";
      return GS_FAILED;
    }
  }
  id[UNIQUEID_CHARS] = 0;
  memcpy(unique_id, id, sizeof(id));
  return GS_OK;
}

static int load_unique_id(const char* keyDirectory) {
  char path[PATH_MAX], temporary[PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/%s", keyDirectory, UNIQUE_FILE_NAME) >=
      (int)sizeof(path)) {
    gs_error = "Moonlight device ID path is too long";
    return GS_FAILED;
  }

  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd >= 0)
    return read_existing_unique_id(fd);
  if (errno != ENOENT) {
    gs_error = "Cannot read Moonlight device ID";
    return GS_FAILED;
  }
  /* Do not confuse a broken symlink with a nonexistent ID. */
  struct stat existing;
  if (lstat(path, &existing) == 0 || errno != ENOENT) {
    gs_error = "Moonlight device ID path is not a regular file";
    return GS_FAILED;
  }

  if (snprintf(temporary, sizeof(temporary), "%s/.uniqueid.XXXXXX",
               keyDirectory) >= (int)sizeof(temporary)) {
    gs_error = "Moonlight device ID temporary path is too long";
    return GS_FAILED;
  }
  fd = mkstemp(temporary); /* Exclusive private 0600 temporary file. */
  if (fd < 0) {
    gs_error = "Cannot create Moonlight device ID";
    return GS_FAILED;
  }
  static const char initial_id[] = "0123456789ABCDEF";
  size_t count = 0;
  bool good = true;
  while (count < UNIQUEID_CHARS) {
    ssize_t n = write(fd, initial_id + count, UNIQUEID_CHARS - count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { good = false; break; }
    count += (size_t)n;
  }
  if (good && fsync(fd) != 0) good = false;
  if (close(fd) != 0) good = false;
  if (!good) {
    unlink(temporary);
    gs_error = "Cannot persist Moonlight device ID";
    return GS_FAILED;
  }
  if (link(temporary, path) != 0) {
    const int failure = errno;
    unlink(temporary);
    if (failure == EEXIST) {
      fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
      if (fd >= 0)
        return read_existing_unique_id(fd);
    }
    gs_error = "Cannot publish Moonlight device ID";
    return GS_FAILED;
  }
  unlink(temporary);
  memcpy(unique_id, initial_id, UNIQUEID_CHARS + 1);
  return GS_OK;
}

static int load_cert(const char* keyDirectory) {
  char certificateFilePath[PATH_MAX];
  snprintf(certificateFilePath, PATH_MAX, "%s/%s", keyDirectory, CERTIFICATE_FILE_NAME);

  char keyFilePath[PATH_MAX];
  snprintf(&keyFilePath[0], PATH_MAX, "%s/%s", keyDirectory, KEY_FILE_NAME);

  FILE *fd = fopen(certificateFilePath, "r");
  if (fd == NULL) {
    if (errno != ENOENT) {
      gs_error = "Cannot read existing client certificate";
      return GS_FAILED;
    }
    printf("Generating certificate...");
    CERT_KEY_PAIR new_pair = mkcert_generate();
    char p12FilePath[PATH_MAX];
    snprintf(p12FilePath, PATH_MAX, "%s/%s", keyDirectory, P12_FILE_NAME);
    if (!mkcert_save(certificateFilePath, p12FilePath, keyFilePath, new_pair)) {
      mkcert_free(new_pair);
      gs_error = "Unable to securely save new client credentials";
      return GS_FAILED;
    }
    mkcert_free(new_pair);
    printf("done\n");
    fd = fopen(certificateFilePath, "r");
  }

  if (fd == NULL) {
    gs_error = "Can't open certificate file";
    return GS_FAILED;
  }

  if (!(cert = PEM_read_X509(fd, NULL, NULL, NULL))) {
    gs_error = "Error loading cert into memory";
    fclose(fd);
    return GS_FAILED;
  }

  rewind(fd);

  /* Each PEM byte expands to two hex characters plus a final NUL. OpenSSL
   * accepts trailing whitespace, so even a valid certificate can exceed
   * this fixed buffer. Reject it rather than truncating or overrunning. */
  int c;
  size_t length = 0;
  while ((c = fgetc(fd)) != EOF) {
    if (length >= sizeof(cert_hex) - 2) {
      gs_error = "Certificate file too large";
      fclose(fd);
      X509_free(cert);
      cert = NULL;
      cert_hex[0] = '\0';
      return GS_FAILED;
    }
    snprintf(cert_hex + length, sizeof(cert_hex) - length, "%02x", (unsigned int)c);
    length += 2;
  }
  if (ferror(fd)) {
    gs_error = "Error reading certificate file";
    fclose(fd);
    X509_free(cert);
    cert = NULL;
    cert_hex[0] = '\0';
    return GS_FAILED;
  }
  cert_hex[length] = '\0';

  fclose(fd);

  fd = fopen(keyFilePath, "r");
  if (fd == NULL) {
    gs_error = "Error loading key into memory";
    return GS_FAILED;
  }

  privateKey = PEM_read_PrivateKey(fd, NULL, NULL, NULL);
  fclose(fd);
  if (!privateKey) {
    gs_error = "Client private key is missing, corrupt, or unreadable";
    return GS_FAILED;
  }
  if (X509_check_private_key(cert, privateKey) != 1) {
    gs_error = "Client private key does not match client certificate";
    EVP_PKEY_free(privateKey);
    privateKey = NULL;
    return GS_FAILED;
  }

  return GS_OK;
}

static int load_serverinfo(PSERVER_DATA server, bool https) {
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  char url[4096];
  int ret = GS_INVALID;
  char *pairedText = NULL;
  char *currentGameText = NULL;
  char *stateText = NULL;
  char *serverCodecModeSupportText = NULL;
  char *httpsPortText = NULL;

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);

  snprintf(url, sizeof(url), "%s://%s:%d/serverinfo?uniqueid=%s&uuid=%s",
    https ? "https" : "http", server->serverInfo.address, https ? server->httpsPort : server->httpPort, unique_id, uuid_str);

  PHTTP_DATA data = http_create_data();
  if (data == NULL) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  if (http_request(url, data) != GS_OK) {
    ret = GS_IO_ERROR;
    goto cleanup;
  }

  if (xml_status(data->memory, data->size) == GS_ERROR) {
    ret = GS_ERROR;
    goto cleanup;
  }

  if (xml_search(data->memory, data->size, "currentgame", &currentGameText) != GS_OK) {
    goto cleanup;
  }

  if (xml_search(data->memory, data->size, "PairStatus", &pairedText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "appversion", (char**) &server->serverInfo.serverInfoAppVersion) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "state", &stateText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "ServerCodecModeSupport", &serverCodecModeSupportText) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "gputype", &server->gpuType) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "GsVersion", &server->gsVersion) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "GfeVersion", (char**) &server->serverInfo.serverInfoGfeVersion) != GS_OK)
    goto cleanup;

  if (xml_search(data->memory, data->size, "HttpsPort", &httpsPortText) != GS_OK)
    goto cleanup;

  if (xml_modelist(data->memory, data->size, &server->modes) != GS_OK)
    goto cleanup;

  // These fields are present on all version of GFE that this client supports
  if (!strlen(currentGameText) || !strlen(pairedText) || !strlen(server->serverInfo.serverInfoAppVersion) || !strlen(stateText))
    goto cleanup;

  server->paired = pairedText != NULL && strcmp(pairedText, "1") == 0;
  server->currentGame = currentGameText == NULL ? 0 : atoi(currentGameText);
  server->serverInfo.serverCodecModeSupport = serverCodecModeSupportText == NULL ? SCM_H264 : atoi(serverCodecModeSupportText);
  server->serverMajorVersion = atoi(server->serverInfo.serverInfoAppVersion);
  server->isNvidiaSoftware = strstr(stateText, "MJOLNIR") != NULL;

  /* The advertised HTTPS port is not trusted merely because the host
   * answered serverinfo. Reject negative, nonnumeric and overflowing ports;
   * absent/empty remains the legacy default for older hosts. */
  if (!moonlight_parse_https_port(httpsPortText, &server->httpsPort)) {
    gs_error = "Sunshine advertised an invalid HTTPS port";
    ret = GS_INVALID;
    goto cleanup;
  }

  if (strstr(stateText, "_SERVER_BUSY") == NULL) {
    // After GFE 2.8, current game remains set even after streaming
    // has ended. We emulate the old behavior by forcing it to zero
    // if streaming is not active.
    server->currentGame = 0;
  }
  ret = GS_OK;

  cleanup:
  if (data != NULL)
    http_free_data(data);

  if (pairedText != NULL)
    free(pairedText);

  if (currentGameText != NULL)
    free(currentGameText);

  if (serverCodecModeSupportText != NULL)
    free(serverCodecModeSupportText);

  if (httpsPortText != NULL)
    free(httpsPortText);

  return ret;
}

static int load_server_status(PSERVER_DATA server) {
  int ret;
  int i;

  /* Fetch the HTTPS port if we don't have one yet */
  if (!server->httpsPort) {
    ret = load_serverinfo(server, false);
    if (ret != GS_OK)
      return ret;
  }

  // Modern GFE versions don't allow serverinfo to be fetched over HTTPS if the client
  // is not already paired. Since we can't pair without knowing the server version, we
  // make another request over HTTP if the HTTPS request fails. We can't just use HTTP
  // for everything because it doesn't accurately tell us if we're paired.
  if (http_server_certificate_pinned()) {
    /* A pinned HTTPS identity mismatch is never grounds to accept an
     * unauthenticated HTTP PairStatus or server status instead. */
    ret = load_serverinfo(server, true);
  } else {
    ret = GS_INVALID;
    for (i = 0; i < 2 && ret != GS_OK; i++)
      ret = load_serverinfo(server, i == 0);
  }

  if (ret == GS_OK && !server->unsupported) {
    if (server->serverMajorVersion > MAX_SUPPORTED_GFE_VERSION) {
      gs_error = "Ensure you're running the latest version of Moonlight Embedded or downgrade GeForce Experience and try again";
      ret = GS_UNSUPPORTED_VERSION;
    } else if (server->serverMajorVersion < MIN_SUPPORTED_GFE_VERSION) {
      gs_error = "Moonlight Embedded requires a newer version of GeForce Experience. Please upgrade GFE on your PC and try again.";
      ret = GS_UNSUPPORTED_VERSION;
    }
  }

  return ret;
}

static void bytes_to_hex(unsigned char *in, char *out, size_t len) {
  for (int i = 0; i < len; i++) {
    sprintf(out + i * 2, "%02x", in[i]);
  }
  out[len * 2] = 0;
}

static bool hex_to_bytes(const char *in, unsigned char *out, size_t len) {
  if (!in || !out || (len & 1) || strlen(in) != len)
    return false;
  if (strspn(in, "0123456789abcdefABCDEF") != len)
    return false;
  for (size_t count = 0; count < len; count += 2) {
    unsigned int octet;
    if (sscanf(in + count, "%2x", &octet) != 1)
      return false;
    out[count / 2] = (unsigned char)octet;
  }
  return true;
}

static int sign_it(const char *msg, size_t mlen, unsigned char **sig, size_t *slen, EVP_PKEY *pkey) {
  int result = GS_FAILED;

  *sig = NULL;
  *slen = 0;

  EVP_MD_CTX *ctx = EVP_MD_CTX_create();
  if (ctx == NULL)
    return GS_FAILED;

  int rc = EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey);
  if (rc != 1)
    goto cleanup;

  rc = EVP_DigestSignUpdate(ctx, msg, mlen);
  if (rc != 1)
    goto cleanup;

  size_t req = 0;
  rc = EVP_DigestSignFinal(ctx, NULL, &req);
  if (rc != 1 || !(req > 0))
    goto cleanup;

  *sig = OPENSSL_malloc(req);
  if (*sig == NULL)
    goto cleanup;

  *slen = req;
  rc = EVP_DigestSignFinal(ctx, *sig, slen);
  if (rc != 1 || req != *slen)
    goto cleanup;

  result = GS_OK;

  cleanup:
  EVP_MD_CTX_destroy(ctx);
  ctx = NULL;

  return result;
}

static bool verifySignature(const char *data, int dataLength, char *signature, int signatureLength, const char *cert) {
  bool verified = false;
  BIO *bio = NULL;
  X509 *x509 = NULL;
  EVP_PKEY *pubKey = NULL;
  EVP_MD_CTX *mdctx = NULL;
  if (!cert || !data || !signature || dataLength < 0 || signatureLength < 1)
    return false;
  bio = BIO_new(BIO_s_mem());
  if (!bio || BIO_puts(bio, cert) <= 0)
    goto cleanup;
  x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  if (!x509) goto cleanup;
  pubKey = X509_get_pubkey(x509);
  mdctx = EVP_MD_CTX_create();
  if (!pubKey || !mdctx) goto cleanup;
  if (EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pubKey) != 1 ||
      EVP_DigestVerifyUpdate(mdctx, data, dataLength) != 1)
    goto cleanup;
  verified = EVP_DigestVerifyFinal(mdctx, signature, signatureLength) == 1;

cleanup:
  EVP_MD_CTX_destroy(mdctx);
  EVP_PKEY_free(pubKey);
  X509_free(x509);
  BIO_free(bio);
  return verified;
}

static void encrypt(const unsigned char *plaintext, int plaintextLen, const unsigned char *key, unsigned char *ciphertext) {
  EVP_CIPHER_CTX* cipher = EVP_CIPHER_CTX_new();

  EVP_EncryptInit(cipher, EVP_aes_128_ecb(), key, NULL);
  EVP_CIPHER_CTX_set_padding(cipher, 0);

  int ciphertextLen = 0;
  EVP_EncryptUpdate(cipher, ciphertext, &ciphertextLen, plaintext, plaintextLen);

  EVP_CIPHER_CTX_free(cipher);
}

static void decrypt(const unsigned char *ciphertext, int ciphertextLen, const unsigned char *key, unsigned char *plaintext) {
  EVP_CIPHER_CTX* cipher = EVP_CIPHER_CTX_new();

  EVP_DecryptInit(cipher, EVP_aes_128_ecb(), key, NULL);
  EVP_CIPHER_CTX_set_padding(cipher, 0);

  int plaintextLen = 0;
  EVP_DecryptUpdate(cipher, plaintext, &plaintextLen, ciphertext, ciphertextLen);

  EVP_CIPHER_CTX_free(cipher);
}

int gs_unpair(PSERVER_DATA server) {
  int ret = GS_OK;
  char url[4096];
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, sizeof(url), "http://%s:%u/unpair?uniqueid=%s&uuid=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str);
  ret = http_request(url, data);
  if (ret == GS_OK)
    ret = xml_status(data->memory, data->size);

  http_free_data(data);
  return ret;
}

int gs_pair(PSERVER_DATA server, char* pin) {
  int ret = GS_OK;
  const char *stage = "preparing pairing";
  char detail[160];
  static char pairing_error[4608];
  gs_error = NULL;
  char* result = NULL;
  size_t url_max_len = 16384;
  char* url = malloc(url_max_len);
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  char* plaincert = NULL;
  char* challenge_response = NULL;
  char* pairing_secret = NULL;
  char* client_pairing_secret = NULL;
  char* client_pairing_secret_hex = NULL;
  unsigned char *signature = NULL;
  size_t signed_length = 0;
  PHTTP_DATA data = NULL;
  bool handshake_started = false;

  if (!url) {
    gs_error = "Out of memory preparing pairing";
    return GS_OUT_OF_MEMORY;
  }

  if (server->paired && http_server_certificate_pinned()) {
    gs_error = "Already paired with a trusted host";
    ret = GS_WRONG_STATE;
    goto cleanup;
  }
  /* A forgotten or missing key pin must be re-established by completing
   * a new PIN handshake, even if the old client ID is still recognized. */

  unsigned char salt_data[16];
  char salt_hex[SIZEOF_AS_HEX_STR(salt_data)];
  if (RAND_bytes(salt_data, sizeof(salt_data)) != 1) {
    gs_error = "Secure pairing salt generation failed";
    ret = GS_FAILED;
    goto cleanup;
  }
  bytes_to_hex(salt_data, salt_hex, sizeof(salt_data));

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&phrase=getservercert&salt=%s&clientcert=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, salt_hex, cert_hex);
  data = http_create_data();
  if (data == NULL) {
    gs_error = "Out of memory preparing pairing response";
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  /* Apollo/Sunshine holds this response until the user submits the PIN.
   * Allow Luna's full 60-second PIN dialog, with a small transport margin.
   * All remaining handshake requests keep the normal 15-second timeout. */
  stage = "waiting for the host certificate after PIN entry";
  if ((ret = http_request_with_timeout(url, data, 65000L)) != GS_OK)
    goto cleanup;

  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  handshake_started = true;
  free(result);
  result = NULL;
  if ((ret = xml_search(data->memory, data->size, "plaincert", &result)) != GS_OK)
    goto cleanup;


  size_t plaincertlen = strlen(result)/2;
  if ((strlen(result) & 1) || plaincertlen < 1 || plaincertlen > 8192 ||
      strspn(result, "0123456789abcdefABCDEF") != strlen(result)) {
    ret = GS_INVALID;
    gs_error = "Malformed pairing certificate";
    goto cleanup;
  }
  plaincert = malloc(plaincertlen + 1);
  if (!plaincert) { ret = GS_OUT_OF_MEMORY; goto cleanup; }
  if (!hex_to_bytes(result, (unsigned char *)plaincert, plaincertlen*2)) {
    ret = GS_INVALID;
    goto cleanup;
  }
  plaincert[plaincertlen] = 0;

  unsigned char salt_pin[sizeof(salt_data) + 4];
  unsigned char aes_key[32]; // Must fit SHA256
  memcpy(salt_pin, salt_data, sizeof(salt_data));
  memcpy(salt_pin+sizeof(salt_data), pin, 4);

  int hash_length = server->serverMajorVersion >= 7 ? 32 : 20;
  if (server->serverMajorVersion >= 7)
    SHA256(salt_pin, sizeof(salt_pin), aes_key);
  else
    SHA1(salt_pin, sizeof(salt_pin), aes_key);

  unsigned char challenge_data[16];
  unsigned char challenge_enc[sizeof(challenge_data)];
  char challenge_hex[SIZEOF_AS_HEX_STR(challenge_enc)];
  if (RAND_bytes(challenge_data, sizeof(challenge_data)) != 1) {
    gs_error = "Secure pairing challenge generation failed";
    ret = GS_FAILED;
    goto cleanup;
  }
  encrypt(challenge_data, sizeof(challenge_data), aes_key, challenge_enc);
  bytes_to_hex(challenge_enc, challenge_hex, sizeof(challenge_enc));

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&clientchallenge=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, challenge_hex);
  stage = "exchanging the pairing challenge";
  printf("Pairing: Host returned its certificate; verifying identity...\n");
  fflush(stdout);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if (xml_search(data->memory, data->size, "challengeresponse", &result) != GS_OK) {
    ret = GS_INVALID;
    goto cleanup;
  }

  char challenge_response_data_enc[64];
  char challenge_response_data[sizeof(challenge_response_data_enc)];

  const size_t response_hex_length = strlen(result);
  const size_t response_length = response_hex_length / 2;
  /* Apollo/Sunshine sends 32 hash bytes + 16 challenge bytes without
   * padding. Retain support for older hosts returning an extra AES block.
   * Never decrypt past the bytes received, or accept a truncated block. */
  if ((response_hex_length & 1) || response_length < (size_t)hash_length + 16 ||
      response_length > sizeof(challenge_response_data_enc) || response_length % 16) {
    snprintf(detail, sizeof(detail), "Invalid server challenge length: received %zu hex characters; expected 96 or 128", response_hex_length);
    gs_error = detail;
    ret = GS_INVALID;
    goto cleanup;
  }
  if (strspn(result, "0123456789abcdefABCDEF") != response_hex_length) {
    gs_error = "Server challenge contains invalid hexadecimal data";
    ret = GS_INVALID;
    goto cleanup;
  }

  if (!hex_to_bytes(result, (unsigned char *)challenge_response_data_enc, strlen(result))) {
    ret = GS_INVALID;
    goto cleanup;
  }
  decrypt(challenge_response_data_enc, (int)response_length, aes_key, challenge_response_data);

  char client_secret_data[16];
  if (RAND_bytes(client_secret_data, sizeof(client_secret_data)) != 1) {
    gs_error = "Secure client pairing secret generation failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  const ASN1_BIT_STRING *asnSignature = NULL;
  X509_get0_signature(&asnSignature, NULL, cert);
  int cert_sig_length = asnSignature ? ASN1_STRING_length(asnSignature) : 0;
  if (cert_sig_length <= 0 || cert_sig_length > 8192) {
    gs_error = "Invalid client certificate signature";
    ret = GS_INVALID;
    goto cleanup;
  }
  challenge_response = malloc(16 + (size_t)cert_sig_length + sizeof(client_secret_data));
  if (!challenge_response) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  char challenge_response_hash[32];
  char challenge_response_hash_enc[sizeof(challenge_response_hash)];
  char challenge_response_hex[SIZEOF_AS_HEX_STR(challenge_response_hash_enc)];
  memcpy(challenge_response, challenge_response_data + hash_length, 16);
  memcpy(challenge_response + 16, ASN1_STRING_get0_data(asnSignature), ASN1_STRING_length(asnSignature));
  memcpy(challenge_response + 16 + ASN1_STRING_length(asnSignature), client_secret_data, sizeof(client_secret_data));
  if (server->serverMajorVersion >= 7)
    SHA256(challenge_response, 16 + ASN1_STRING_length(asnSignature) + sizeof(client_secret_data), challenge_response_hash);
  else
    SHA1(challenge_response, 16 + ASN1_STRING_length(asnSignature) + sizeof(client_secret_data), challenge_response_hash);

  encrypt(challenge_response_hash, sizeof(challenge_response_hash), aes_key, challenge_response_hash_enc);
  bytes_to_hex(challenge_response_hash_enc, challenge_response_hex, sizeof(challenge_response_hash_enc));

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&serverchallengeresp=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, challenge_response_hex);
  stage = "verifying the host pairing secret";
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;
  if (xml_search(data->memory, data->size, "pairingsecret", &result) != GS_OK) {
    ret = GS_INVALID;
    goto cleanup;
  }

  size_t pairing_secret_len = strlen(result) / 2;
  if ((strlen(result) & 1) || pairing_secret_len <= 16 ||
      pairing_secret_len > 8192 ||
      strspn(result, "0123456789abcdefABCDEF") != strlen(result)) {
    ret = GS_INVALID;
    goto cleanup;
  }

  pairing_secret = malloc(pairing_secret_len);
  if (!pairing_secret) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  if (!hex_to_bytes(result, (unsigned char *)pairing_secret, pairing_secret_len*2)) {
    ret = GS_INVALID;
    goto cleanup;
  }
  if (!verifySignature(pairing_secret, 16, pairing_secret+16, pairing_secret_len-16, plaincert)) {
    gs_error = "Host pairing signature verification failed; host identity could not be verified";
    ret = GS_FAILED;
    goto cleanup;
  }

  /* The PIN-verified certificate is now trusted for the final HTTPS phase.
   * Do not accept an arbitrary HTTPS certificate merely because it connects. */
  if ((ret = http_prepare_paired_certificate(plaincert)) != GS_OK)
    goto cleanup;

  if (sign_it(client_secret_data, sizeof(client_secret_data), &signature, &signed_length, privateKey) != GS_OK) {
      gs_error = "Failed to sign data";
      ret = GS_FAILED;
      goto cleanup;
  }

  if (!signed_length || signed_length > 8192) {
    gs_error = "Unexpected client pairing signature length";
    ret = GS_INVALID;
    goto cleanup;
  }
  const size_t secret_length = sizeof(client_secret_data) + signed_length;
  client_pairing_secret = malloc(secret_length);
  client_pairing_secret_hex = malloc(LEN_AS_HEX_STR(secret_length));
  if (!client_pairing_secret || !client_pairing_secret_hex) {
    ret = GS_OUT_OF_MEMORY;
    goto cleanup;
  }
  memcpy(client_pairing_secret, client_secret_data, sizeof(client_secret_data));
  memcpy(client_pairing_secret + sizeof(client_secret_data), signature, signed_length);
  bytes_to_hex(client_pairing_secret, client_pairing_secret_hex, secret_length);

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, url_max_len, "http://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&clientpairingsecret=%s", server->serverInfo.address, server->httpPort, unique_id, uuid_str, client_pairing_secret_hex);
  stage = "confirming the client pairing secret";
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, url_max_len, "https://%s:%u/pair?uniqueid=%s&uuid=%s&devicename=roth&updateState=1&phrase=pairchallenge", server->serverInfo.address, server->httpsPort, unique_id, uuid_str);
  stage = "confirming pairing over HTTPS";
  printf("Pairing: Confirming pairing over HTTPS...\n");
  fflush(stdout);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  free(result);
  result = NULL;
  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "paired", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "1") != 0) {
    gs_error = "Pairing failed";
    ret = GS_FAILED;
    goto cleanup;
  }

  stage = "saving the verified host certificate";
  printf("Pairing: Saving verified host identity...\n");
  fflush(stdout);
  ret = http_commit_paired_certificate();
  if (ret == GS_OK) server->paired = true;

  cleanup:
  if (ret != GS_OK) {
    /* Copy before rollback: HTTP/XML helpers reuse their error buffers. A
     * currentGame ID can also represent a desktop or stale host session;
     * it must never replace the actual transport/protocol failure. */
    const char *reason = gs_error ? gs_error :
        ret == GS_OUT_OF_MEMORY ? "Out of memory" : "Invalid or incomplete host response";
    snprintf(pairing_error, sizeof(pairing_error), "%s: %s", stage, reason);
  }
  if (ret != GS_OK && handshake_started) {
    /* Only roll back an attempt after this call began a pairing handshake.
     * Pre-existing credentials survive a rejected Pair request. */
    gs_unpair(server);
  }
  if (ret != GS_OK) gs_error = pairing_error;

  http_discard_pending_certificate();
  OPENSSL_free(signature);
  free(url);
  free(plaincert);
  free(challenge_response);
  free(pairing_secret);
  free(client_pairing_secret);
  free(client_pairing_secret_hex);
  free(result);

  if (data)
    http_free_data(data);

  return ret;
}

int gs_applist(PSERVER_DATA server, PAPP_LIST *list) {
  int ret = GS_OK;
  char url[4096];
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, sizeof(url), "https://%s:%u/applist?uniqueid=%s&uuid=%s", server->serverInfo.address, server->httpsPort, unique_id, uuid_str);
  if (http_request(url, data) != GS_OK)
    ret = GS_IO_ERROR;
  else if (xml_status(data->memory, data->size) == GS_ERROR)
    ret = GS_ERROR;
  else if (xml_applist(data->memory, data->size, list) != GS_OK)
    ret = GS_INVALID;

  http_free_data(data);
  return ret;
}

int gs_start_app(PSERVER_DATA server, STREAM_CONFIGURATION *config, int appId, bool sops, bool localaudio, int gamepad_mask) {
  int ret = GS_OK;
  uuid_t uuid;
  char* result = NULL;
  char uuid_str[UUID_STRLEN];

  PDISPLAY_MODE mode = server->modes;
  bool correct_mode = false;
  while (mode != NULL) {
    if (mode->width == config->width && mode->height == config->height) {
      if (mode->refresh == config->fps)
        correct_mode = true;
    }

    mode = mode->next;
  }

  if (!correct_mode && !server->unsupported)
    return GS_NOT_SUPPORTED_MODE;

  if (RAND_bytes(config->remoteInputAesKey, sizeof(config->remoteInputAesKey)) != 1) {
    gs_error = "Secure streaming key generation failed";
    return GS_FAILED;
  }
  memset(config->remoteInputAesIv, 0, sizeof(config->remoteInputAesIv));

  char url[4096];
  u_int32_t rikeyid = 0;
  if (RAND_bytes(config->remoteInputAesIv, sizeof(rikeyid)) != 1) {
    gs_error = "Secure streaming IV generation failed";
    return GS_FAILED;
  }
  memcpy(&rikeyid, config->remoteInputAesIv, sizeof(rikeyid));
  rikeyid = htonl(rikeyid);
  char rikey_hex[SIZEOF_AS_HEX_STR(config->remoteInputAesKey)];
  bytes_to_hex(config->remoteInputAesKey, rikey_hex, sizeof(config->remoteInputAesKey));

  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  // Using an FPS value over 60 causes SOPS to default to 720p60,
  // so force it to 0 to ensure the correct resolution is set. We
  // used to use 60 here but that locked the frame rate to 60 FPS
  // on GFE 3.20.3.
  int fps = (server->isNvidiaSoftware && config->fps > 60) ? 0 : config->fps;

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  int surround_info = SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(config->audioConfiguration);
  snprintf(url, sizeof(url), "https://%s:%u/%s?uniqueid=%s&uuid=%s&appid=%d&mode=%dx%dx%d&additionalStates=1&sops=%d&rikey=%s&rikeyid=%d&localAudioPlayMode=%d&surroundAudioInfo=%d&remoteControllersBitmap=%d&gcmap=%d%s%s",
           server->serverInfo.address, server->httpsPort, server->currentGame ? "resume" : "launch", unique_id, uuid_str, appId, config->width, config->height, fps, sops, rikey_hex, rikeyid, localaudio, surround_info, gamepad_mask, gamepad_mask,
           (config->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ? "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" : "",
           LiGetLaunchUrlQueryParameters());
  if ((ret = http_request(url, data)) == GS_OK)
    server->currentGame = appId;
  else
    goto cleanup;

  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "gamesession", &result)) != GS_OK &&
           (ret = xml_search(data->memory, data->size, "resume", &result)) != GS_OK)
    goto cleanup;

  if (!strcmp(result, "0")) {
    ret = GS_FAILED;
    goto cleanup;
  }

  free(result);
  result = NULL;

  if (xml_search(data->memory, data->size, "sessionUrl0", &result) == GS_OK) {
    server->serverInfo.rtspSessionUrl = result;
    result = NULL;
  }

  cleanup:
  if (result != NULL)
    free(result);

  http_free_data(data);
  return ret;
}

int gs_quit_app(PSERVER_DATA server) {
  int ret = GS_OK;
  char url[4096];
  uuid_t uuid;
  char uuid_str[UUID_STRLEN];
  char* result = NULL;
  PHTTP_DATA data = http_create_data();
  if (data == NULL)
    return GS_OUT_OF_MEMORY;

  uuid_generate_random(uuid);
  uuid_unparse(uuid, uuid_str);
  snprintf(url, sizeof(url), "https://%s:%u/cancel?uniqueid=%s&uuid=%s", server->serverInfo.address, server->httpsPort, unique_id, uuid_str);
  if ((ret = http_request(url, data)) != GS_OK)
    goto cleanup;

  if ((ret = xml_status(data->memory, data->size)) != GS_OK)
    goto cleanup;
  else if ((ret = xml_search(data->memory, data->size, "cancel", &result)) != GS_OK)
    goto cleanup;

  if (strcmp(result, "0") == 0) {
    ret = GS_FAILED;
    goto cleanup;
  }

  cleanup:
  if (result != NULL)
    free(result);

  http_free_data(data);
  return ret;
}

int gs_forget_server_pin(const char *keyDirectory, const char *address) {
  return http_forget_server_certificate(keyDirectory, address);
}

bool gs_server_identity_verified(void) {
  return http_server_certificate_pinned();
}

/* The local keys were historically created under the process umask.
 * Repair existing permissions and refuse symlinks/foreign-owned key files
 * before trusting or generating this client identity. */
static int protect_client_identity(const char *keyDirectory) {
  const char *files[] = {CERTIFICATE_FILE_NAME, KEY_FILE_NAME, P12_FILE_NAME};
  struct stat st;
  if (lstat(keyDirectory, &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != geteuid() || chmod(keyDirectory, 0700) != 0) {
    gs_error = "Client credential directory cannot be secured";
    return GS_FAILED;
  }
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", keyDirectory, files[i]) >=
        (int)sizeof(path)) {
      gs_error = "Client credential path is too long";
      return GS_FAILED;
    }
    if (lstat(path, &st) != 0) {
      if (errno == ENOENT) continue; /* Legacy P12 may not exist. */
      gs_error = "Cannot inspect client credential permissions";
      return GS_FAILED;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        chmod(path, 0600) != 0) {
      gs_error = "Client credential file cannot be secured";
      return GS_FAILED;
    }
  }
  return GS_OK;
}

int gs_init(PSERVER_DATA server, char *address, unsigned short httpPort, const char *keyDirectory, int log_level, bool unsupported) {
  if (mkdirtree(keyDirectory) != 0) {
    gs_error = "Unable to create client credential directory";
    return GS_FAILED;
  }
  if (protect_client_identity(keyDirectory) != GS_OK)
    return GS_FAILED;
  if (load_unique_id(keyDirectory) != GS_OK)
    return GS_FAILED;

  if (load_cert(keyDirectory))
    return GS_FAILED;

  if (http_init(keyDirectory, log_level) != GS_OK)
    return GS_FAILED;
  if (http_bind_server_certificate(keyDirectory, address) != GS_OK)
    return GS_FAILED;

  LiInitializeServerInformation(&server->serverInfo);
  server->serverInfo.address = address;
  server->unsupported = unsupported;
  server->httpPort = httpPort ? httpPort : 47989;
  server->httpsPort = 0; /* Populated by load_server_status() */
  int status = load_server_status(server);
  if (status == GS_OK && server->paired && !http_server_certificate_pinned())
    fprintf(stderr, "Sunshine HTTPS host identity is unverified; re-pair to enable certificate pinning\n");
  return status;
}
