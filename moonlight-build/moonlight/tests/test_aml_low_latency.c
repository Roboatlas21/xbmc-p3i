#include "../src/video/aml_low_latency.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  char value[8];
  int reads;
  int writes;
  int fail_write_at;
  int fail_read_at;
} Fake;

static int mock_read(void *ctx, const char *path, char *out, size_t size) {
  Fake *f = ctx;
  (void)path;
  f->reads++;
  if (f->reads == f->fail_read_at) return -1;
  if (!f->value[0] || strlen(f->value) >= size) return -1;
  strcpy(out, f->value);
  return 0;
}

static int mock_write(void *ctx, const char *path, const char *value) {
  Fake *f = ctx;
  (void)path;
  f->writes++;
  if (f->writes == f->fail_write_at) return -1;
  strcpy(f->value, value);
  return 0;
}

static void init(Fake *f, AmlLowLatency *state, const char *value) {
  memset(f, 0, sizeof(*f));
  strcpy(f->value, value);
  AmlLowLatencyIo io = {
    .ctx = f,
    .read_text = mock_read,
    .write_text = mock_write,
  };
  aml_low_latency_init(state, &io);
}

int main(void) {
  Fake f;
  AmlLowLatency state;

  init(&f, &state, "0");
  assert(aml_low_latency_enable(&state));
  assert(state.changed);
  assert(!strcmp(state.original, "0"));
  assert(!strcmp(f.value, "1"));
  aml_low_latency_restore(&state);
  assert(!state.changed);
  assert(!strcmp(f.value, "0"));

  init(&f, &state, "1");
  assert(aml_low_latency_enable(&state));
  assert(!state.changed);
  assert(f.writes == 0);
  aml_low_latency_restore(&state);
  assert(f.writes == 0);

  init(&f, &state, "2");
  assert(!aml_low_latency_enable(&state));
  assert(!state.changed);
  assert(f.writes == 0);

  init(&f, &state, "0");
  f.fail_write_at = 1;
  assert(!aml_low_latency_enable(&state));
  assert(!state.changed);
  assert(!strcmp(f.value, "0"));

  init(&f, &state, "0");
  assert(aml_low_latency_enable(&state));
  strcpy(f.value, "0"); /* external actor changed it */
  const int writes_before_restore = f.writes;
  aml_low_latency_restore(&state);
  assert(!state.changed);
  assert(f.writes == writes_before_restore);
  assert(!strcmp(f.value, "0"));

  init(&f, &state, "0");
  assert(aml_low_latency_enable(&state));
  f.fail_write_at = f.writes + 1;
  aml_low_latency_restore(&state);
  assert(state.changed); /* failed restore remains retryable */
  assert(!strcmp(f.value, "1"));

  init(&f, &state, "0");
  assert(aml_low_latency_enable(&state));
  f.fail_read_at = f.reads + 1;
  aml_low_latency_restore(&state);
  assert(state.changed); /* unreadable restore also remains retryable */

  puts("Amlogic use_low_latency save/restore tests passed");
  return 0;
}
