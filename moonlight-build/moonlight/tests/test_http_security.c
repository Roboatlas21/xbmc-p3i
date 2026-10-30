/* Exercise the production HTTP response callback and certificate persistence.
 * This test never connects to a real host. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../libgamestream/http.c"

const char *gs_error;

static char *read_pem(const char *path) {
  FILE *f = fopen(path, "rb");
  assert(f);
  char *out = calloc(1, 8193);
  assert(out);
  size_t bytes = fread(out, 1, 8192, f);
  assert(bytes && bytes < 8192);
  fclose(f);
  return out;
}

int main(int argc, char **argv) {
  assert(argc == 3);
  HTTP_DATA out = {.memory = calloc(1, 1), .size = 0};
  assert(out.memory);
  assert(_write_curl("hello", 1, 5, &out) == 5);
  assert(out.size == 5 && !strcmp(out.memory, "hello"));
  /* No allocation/copy should happen after the response reaches its cap. */
  out.size = HTTP_MAX_RESPONSE_BYTES;
  assert(_write_curl("x", 1, 1, &out) == 0);
  assert(out.size == HTTP_MAX_RESPONSE_BYTES);
  out.size = 0;
  assert(_write_curl("x", (size_t)-1, 2, &out) == 0);
  free(out.memory);
  HTTP_DATA cancelled = {.memory = calloc(1, 1), .size = 0};
  assert(cancelled.memory);
  http_cancel_pending_requests();
  assert(http_request_with_timeout("http://invalid", &cancelled, 1000L) == GS_FAILED);
  http_reset_pending_cancellation();
  free(cancelled.memory);

  char *pem = read_pem(argv[2]);
  assert(http_bind_server_certificate(argv[1], "192.168.1.17") == GS_OK);
  assert(!http_server_certificate_pinned());
  assert(http_prepare_paired_certificate(pem) == GS_OK);
  assert(http_server_certificate_pinned());
  assert(http_commit_paired_certificate() == GS_OK);
  assert(http_bind_server_certificate(argv[1], "192.168.1.17") == GS_OK);
  assert(http_server_certificate_pinned());
  /* A deliberate reset must only remove this host's pin, never client keys. */
  assert(http_forget_server_certificate(argv[1], "192.168.1.17") == GS_OK);
  assert(!http_server_certificate_pinned());
  assert(http_bind_server_certificate(argv[1], "192.168.1.17") == GS_OK);
  assert(!http_server_certificate_pinned());
  assert(http_forget_server_certificate(argv[1], "192.168.1.17") == GS_OK);
  assert(http_bind_server_certificate(argv[1], "192.168.1.18") == GS_OK);
  assert(!http_server_certificate_pinned());
  free(pem);
  puts("PASS: response bounds and per-host PIN-authenticated TLS trust");
  return 0;
}
