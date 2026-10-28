/*
 * Amlogic read-only VFM probe and automatic HEVC configuration.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HEVC tuning writes global kernel settings. Refuse unknown configurations
 * and restore the original state on normal exit. VFM is never written here;
 * frame routing is selected by libamcodec's explicit video_path.
 */
#include "aml_tuning.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VFM_PATH "/sys/class/vfm/map"
#define SKIP_PATH "/sys/module/amvdec_h265/parameters/nal_skip_policy"
#define REF_PATH "/sys/module/amvdec_h265/parameters/ref_frame_mark_flag"
#define ALL_ZEROS "0,0,0,0,0,0,0,0,0"
#define ALL_ONES "1,1,1,1,1,1,1,1,1"

static int sysfs_read(void *unused, const char *path, char *text, size_t size) {
  (void) unused;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = read(fd, text, size - 1);
  int saved_errno = errno;
  close(fd);
  if (n < 1 || (size_t)n == size - 1) { errno = n < 0 ? saved_errno : EOVERFLOW; return -1; }
  text[n] = '\0';
  while (n > 0 && isspace((unsigned char)text[n - 1])) text[--n] = '\0';
  return 0;
}

static int sysfs_write(void *unused, const char *path, const char *value) {
  (void) unused;
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  size_t size = strlen(value);
  ssize_t n = write(fd, value, size); // One atomic sysfs command, no O_TRUNC.
  int saved_errno = errno;
  int closed = close(fd);
  if (n != (ssize_t)size || closed < 0) { errno = n < 0 ? saved_errno : EIO; return -1; }
  return 0;
}

void aml_tuning_init(AmlTuning *state, const AmlTuningIo *io) {
  memset(state, 0, sizeof(*state));
  if (io) state->io = *io;
  else {
    state->io.read_text = sysfs_read;
    state->io.write_text = sysfs_write;
  }
}

static int read_value(AmlTuning *s, const char *path, char *buffer, size_t cap) {
  if (!s->io.read_text || s->io.read_text(s->io.ctx, path, buffer, cap) != 0)
    return -1;
  size_t len = strlen(buffer);
  while (len && isspace((unsigned char)buffer[len-1])) buffer[--len] = '\0';
  return 0;
}

/* Parse only the default VFM entry, not provider or receiver lists.
 * Reject malformed and duplicate mappings. Report active providers. */
static int get_default_chain(const char *map, char *out, size_t cap, bool *active) {
  const char *line = map;
  int count = 0;
  *active = false;
  while (*line) {
    const char *end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    const char *brace = memchr(line, '{', (size_t)(end - line));
    if (brace && memchr(line, ']', (size_t)(brace - line))) {
      const char *id = strchr(line, ']');
      if (id && id < brace) {
        id++;
        while (id < brace && isspace((unsigned char)*id)) id++;
        const char *stop = id;
        while (stop < brace && !isspace((unsigned char)*stop)) stop++;
        if ((size_t)(stop - id) == 7 && strncmp(id, "default", 7) == 0) {
          if (++count != 1) return -1;
          const char *closing = memchr(brace, '}', (size_t)(end - brace));
          if (!closing) return -1;
          char *next = out;
          bool space = false;
          for (const char *p = brace + 1; p < closing; p++) {
            if (*p == '(') {
              const char *q = memchr(p, ')', (size_t)(closing - p));
              if (!q || q-p != 2 || (p[1] != '0' && p[1] != '1')) return -1;
              if (p[1] == '1') *active = true;
              p = q;
              continue;
            }
            if (isspace((unsigned char)*p)) { space = (next != out); continue; }
            if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-')) return -1;
            if (space) {
              if ((size_t)(next - out) >= cap - 1) return -1;
              *next++ = ' ';
              space = false;
            }
            if ((size_t)(next - out) >= cap - 1) return -1;
            *next++ = *p;
          }
          *next = '\0';
        }
      }
    }
    line = *end ? end + 1 : end;
  }
  return count == 1 ? 0 : -1;
}

static int read_default(AmlTuning *state, char *chain, size_t cap, bool *active) {
  char raw[4096];
  if (read_value(state, VFM_PATH, raw, sizeof(raw)) != 0) return -1;
  return get_default_chain(raw, chain, cap, active);
}

static bool is_set(const char *setting) { return setting && strcmp(setting, "1") == 0; }

/* Restore only if another process has not changed the tuned value. */
static void restore_param(AmlTuning *s, const char *path, const char *expected,
                          const char *original, bool *active) {
  if (!*active) return;
  char current[128];
  if (read_value(s, path, current, sizeof(current)) != 0 || strcmp(current, expected) != 0) {
    fprintf(stderr, "AML TUNING: %s changed externally or unreadable; not overwriting\n", path);
    *active = false;
    return;
  }
  if (s->io.write_text(s->io.ctx, path, original) != 0) {
    fprintf(stderr, "AML TUNING: failed restoring %s (manual recovery may be required)\n", path);
    return;
  }
  char check[128];
  if (read_value(s, path, check, sizeof(check)) != 0 ||
      strcmp(check, original) != 0) {
    fprintf(stderr, "AML TUNING: restoration not confirmed for %s\n", path);
    return;
  }
  *active = false;
}

void aml_tuning_restore(AmlTuning *s) {
  restore_param(s, REF_PATH, ALL_ZEROS, s->original_ref, &s->ref_active);
  restore_param(s, SKIP_PATH, "0", s->original_skip, &s->skip_active);
}

static void apply_hevc(AmlTuning *s) {
  char skip[64], ref[128];
  if (read_value(s, SKIP_PATH, skip, sizeof(skip)) ||
      read_value(s, REF_PATH, ref, sizeof(ref))) {
    fprintf(stderr, "AML TUNING: HEVC sysfs parameters unavailable; skipped\n");
    return;
  }
  if (strcmp(skip, "2") != 0 || strcmp(ref, ALL_ONES) != 0) {
    fprintf(stderr, "AML TUNING: HEVC settings differ from known NG defaults; skipped\n");
    return;
  }
  strcpy(s->original_skip, skip);
  strcpy(s->original_ref, ref);
  if (s->io.write_text(s->io.ctx, SKIP_PATH, "0") != 0) return;
  s->skip_active = true;
  if (s->io.write_text(s->io.ctx, REF_PATH, ALL_ZEROS) == 0) s->ref_active = true;
  char verify_skip[64], verify_ref[128];
  if (!s->ref_active || read_value(s, SKIP_PATH, verify_skip, sizeof(verify_skip)) ||
      read_value(s, REF_PATH, verify_ref, sizeof(verify_ref)) ||
      strcmp(verify_skip, "0") != 0 || strcmp(verify_ref, ALL_ZEROS) != 0) {
    fprintf(stderr, "AML TUNING: HEVC readback failed; attempting rollback\n");
    aml_tuning_restore(s);
  }
}

void aml_tuning_configure(AmlTuning *s, bool hevc, const char *probe) {
  if (is_set(probe)) {
    char current[128];
    bool active = false;
    if (read_default(s, current, sizeof(current), &active) == 0)
      fprintf(stderr, "AML TUNING: default VFM chain: %s (active=%d)\n",
              current, (int) active);
    else fprintf(stderr, "AML TUNING: VFM default unavailable or active\n");
    if (hevc) {
      char skip[64], ref[128];
      if (!read_value(s, SKIP_PATH, skip, sizeof(skip)) &&
          !read_value(s, REF_PATH, ref, sizeof(ref)))
        fprintf(stderr, "AML TUNING: HEVC skip=%s ref=%s\n", skip, ref);
      else fprintf(stderr, "AML TUNING: HEVC sysfs parameters unavailable\n");
    }
  }
  if (hevc) apply_hevc(s);
}
