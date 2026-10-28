#include "../src/video/aml_tuning.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  char vfm[1024];
  char skip[64];
  char ref[128];
  int writes;
  int fail_at;
} Fake;

static int mock_read(void *ctx, const char *path, char *out, size_t size) {
  Fake *f = ctx;
  const char *value = strstr(path, "/vfm/map") ? f->vfm :
    strstr(path, "nal_skip_policy") ? f->skip :
    strstr(path, "ref_frame_mark_flag") ? f->ref : NULL;
  if (!value || !*value || strlen(value) >= size) return -1;
  strcpy(out, value);
  return 0;
}

static int mock_write(void *ctx, const char *path, const char *value) {
  Fake *f = ctx;
  f->writes++;
  if (f->writes == f->fail_at) return -1;
  if (strstr(path, "nal_skip_policy")) { strcpy(f->skip, value); return 0; }
  if (strstr(path, "ref_frame_mark_flag")) { strcpy(f->ref, value); return 0; }
  /* VFM is intentionally read-only. */
  if (strstr(path, "/vfm/map")) return -1;
  return -1;
}

static void reset(Fake *f, AmlTuning *state) {
  memset(f, 0, sizeof(*f));
  strcpy(f->vfm, "[00]  default { decoder(0) amvideo }\n[01]  dvpath { dvdv(0) amvideo }\n");
  strcpy(f->skip, "2");
  strcpy(f->ref, "1,1,1,1,1,1,1,1,1");
  AmlTuningIo io = {.ctx=f, .read_text=mock_read, .write_text=mock_write};
  aml_tuning_init(state, &io);
}

int main(void) {
  AmlTuning state;
  Fake f;

  reset(&f, &state);
  aml_tuning_configure(&state, true, NULL);
  assert(state.skip_active && state.ref_active);
  assert(!strcmp(f.skip, "0"));
  assert(!strcmp(f.ref, "0,0,0,0,0,0,0,0,0"));
  aml_tuning_restore(&state);
  assert(!strcmp(f.skip, "2"));
  assert(!strcmp(f.ref, "1,1,1,1,1,1,1,1,1"));
  assert(!state.skip_active && !state.ref_active);

  reset(&f, &state);
  aml_tuning_configure(&state, true, "1");
  assert(state.skip_active && state.ref_active);
  assert(f.writes == 2); /* Probe remains read-only; HEVC tuning is automatic. */
  aml_tuning_restore(&state);

  reset(&f, &state);
  aml_tuning_configure(&state, false, NULL);
  assert(!f.writes); /* HEVC-only tuning must not apply to H.264. */

  reset(&f, &state);
  strcpy(f.skip, "3");
  aml_tuning_configure(&state, true, NULL);
  assert(!f.writes); /* Unknown defaults are refused. */

  reset(&f, &state);
  f.fail_at = 2; /* ref write fails; skip is rolled back */
  aml_tuning_configure(&state, true, NULL);
  assert(!strcmp(f.skip, "2"));
  assert(!state.skip_active && !state.ref_active);

  reset(&f, &state);
  aml_tuning_configure(&state, true, NULL);
  strcpy(f.skip, "3"); /* another process changed setting */
  aml_tuning_restore(&state);
  assert(!strcmp(f.skip, "3"));
  assert(!strcmp(f.ref, "1,1,1,1,1,1,1,1,1"));

  puts("All Amlogic probe/automatic HEVC tuning tests passed");
  return 0;
}
