/*
 * Temporary Amlogic decoder low-latency kernel tuning.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "aml_low_latency.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define AML_LOW_LATENCY_PATH "/sys/module/aml_media/parameters/use_low_latency"

static int sysfs_read(void *unused, const char *path, char *text, size_t size) {
  (void)unused;
  if (!text || size < 2) return -1;

  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;

  const ssize_t n = read(fd, text, size - 1);
  const int saved_errno = errno;
  close(fd);
  if (n < 1 || (size_t)n >= size - 1) {
    errno = n < 0 ? saved_errno : EOVERFLOW;
    return -1;
  }

  text[n] = '\0';
  size_t len = (size_t)n;
  while (len && isspace((unsigned char)text[len - 1]))
    text[--len] = '\0';
  return 0;
}

static int sysfs_write(void *unused, const char *path, const char *text) {
  (void)unused;
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;

  const size_t len = strlen(text);
  const ssize_t n = write(fd, text, len);
  const int saved_errno = errno;
  const int close_result = close(fd);
  if (n != (ssize_t)len || close_result < 0) {
    errno = n < 0 ? saved_errno : EIO;
    return -1;
  }
  return 0;
}

static int read_value(AmlLowLatency *state, char *out, size_t size) {
  if (!state || !state->io.read_text ||
      state->io.read_text(state->io.ctx, AML_LOW_LATENCY_PATH, out, size) != 0)
    return -1;

  return (strcmp(out, "0") == 0 || strcmp(out, "1") == 0) ? 0 : -1;
}

void aml_low_latency_init(AmlLowLatency *state, const AmlLowLatencyIo *io) {
  memset(state, 0, sizeof(*state));
  if (io) {
    state->io = *io;
  } else {
    state->io.read_text = sysfs_read;
    state->io.write_text = sysfs_write;
  }
}

bool aml_low_latency_enable(AmlLowLatency *state) {
  char current[8];
  if (read_value(state, current, sizeof(current)) != 0) {
    fprintf(stderr,
            "AML: use_low_latency unavailable or has an unexpected value; skipped\n");
    return false;
  }

  strcpy(state->original, current);
  state->changed = false;

  if (strcmp(current, "1") == 0) {
    fprintf(stderr, "AML: decoder low-latency mode already enabled\n");
    return true;
  }

  if (!state->io.write_text ||
      state->io.write_text(state->io.ctx, AML_LOW_LATENCY_PATH, "1") != 0) {
    fprintf(stderr, "AML: failed to enable decoder low-latency mode\n");
    return false;
  }

  state->changed = true;

  char verify[8];
  if (read_value(state, verify, sizeof(verify)) != 0 ||
      strcmp(verify, "1") != 0) {
    fprintf(stderr,
            "AML: decoder low-latency mode enable was not confirmed; restoring\n");
    aml_low_latency_restore(state);
    return false;
  }

  fprintf(stderr, "AML: decoder low-latency mode enabled\n");
  return true;
}

void aml_low_latency_restore(AmlLowLatency *state) {
  if (!state || !state->changed)
    return;

  char current[8];
  if (read_value(state, current, sizeof(current)) != 0) {
    fprintf(stderr,
            "AML: use_low_latency unreadable during restore; will retry on cleanup\n");
    return;
  }

  if (strcmp(current, "1") != 0) {
    fprintf(stderr,
            "AML: use_low_latency changed externally; not overwriting\n");
    state->changed = false;
    return;
  }

  if (!state->io.write_text ||
      state->io.write_text(state->io.ctx, AML_LOW_LATENCY_PATH,
                           state->original) != 0) {
    fprintf(stderr,
            "AML: failed restoring use_low_latency to %s\n", state->original);
    return;
  }

  char verify[8];
  if (read_value(state, verify, sizeof(verify)) != 0 ||
      strcmp(verify, state->original) != 0) {
    fprintf(stderr,
            "AML: use_low_latency restoration not confirmed (wanted %s)\n",
            state->original);
    return;
  }

  state->changed = false;
  fprintf(stderr, "AML: decoder low-latency mode restored to %s\n",
          state->original);
}
