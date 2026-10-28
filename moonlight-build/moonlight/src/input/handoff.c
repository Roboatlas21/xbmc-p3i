/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "handoff.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PROC_PIDS 512
#define MAX_PROC_FDS 256
#define MAX_REPORTS 12

bool input_handoff_enabled(const char *env_key) {
  const char *value = getenv(env_key);
  return value && strcmp(value, "1") == 0;
}

bool input_handoff_event_path(const char *path) {
  if (!path || strncmp(path, "/dev/input/event", 16) != 0) return false;
  const char *p = path + 16;
  if (!isdigit((unsigned char)*p)) return false;
  while (isdigit((unsigned char)*p)) p++;
  return *p == '\0';
}

bool input_handoff_retryable(int error, unsigned attempt) {
  return attempt < 3 &&
         (error == EBUSY || error == EAGAIN || error == EACCES);
}

static bool decimal_name(const char *s) {
  if (!s || !*s) return false;
  for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
  return true;
}

static bool matches_selected(const char *path, const char *const *selected, int count) {
  if (!input_handoff_event_path(path)) return false;
  if (count == 0) return true; /* Auto-input mode: report event devices only. */
  if (!selected) return false;
  for (int i=0; i<count; i++)
    if (selected[i] && strcmp(path, selected[i]) == 0) return true;
  return false;
}

void input_handoff_probe(const char *const *selected, int count) {
  if (count < 0 || count > 6) return;
  DIR *proc = opendir("/proc");
  if (!proc) {
    fprintf(stderr, "INPUT_HANDOFF: /proc unavailable; inspection skipped\n");
    return;
  }
  unsigned pids = 0, seen = 0, printed = 0, pids_with_input = 0;
  pid_t self = getpid();
  struct dirent *entry;
  while ((entry = readdir(proc)) != NULL && pids < MAX_PROC_PIDS) {
    if (!decimal_name(entry->d_name)) continue;
    const long pid = strtol(entry->d_name, NULL, 10);
    if (pid <= 0 || pid == (long)self) continue;
    pids++;
    char folder[80];
    snprintf(folder, sizeof(folder), "/proc/%ld/fd", pid);
    DIR *fds = opendir(folder);
    if (!fds) continue; /* Permission restriction or process exited. */
    unsigned checked = 0;
    bool found = false;
    struct dirent *fd;
    while ((fd = readdir(fds)) != NULL && checked < MAX_PROC_FDS) {
      if (!decimal_name(fd->d_name)) continue;
      checked++;
      char descriptor[360], target[256];
      snprintf(descriptor, sizeof(descriptor), "%s/%s", folder, fd->d_name);
      const ssize_t n = readlink(descriptor, target, sizeof(target)-1);
      if (n <= 0 || n >= (ssize_t)sizeof(target)) continue;
      target[n] = '\0';
      if (!matches_selected(target, selected, count)) continue;
      seen++;
      found = true;
      if (printed++ < MAX_REPORTS)
        fprintf(stderr, "INPUT_HANDOFF: %s open by pid %ld (NOT proof of EVIOCGRAB)\n",
                target, pid);
    }
    closedir(fds);
    if (found) pids_with_input++;
  }
  closedir(proc);
  fprintf(stderr, "INPUT_HANDOFF_V1 selected=%d other_pids=%u open_descriptors=%u scanned_pids=%u "
          "(read-only, exclusive grabs unknown)\n", count, pids_with_input, seen, pids);
}
