/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_config_path.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *moonlight_host_config_path(const char *address) {
  if (!address) return NULL;
  const size_t len = strlen(address);
  const size_t extra = sizeof("hosts/") + sizeof(".conf");
  if (len > SIZE_MAX - extra) return NULL;
  char *path = malloc(len + extra);
  if (!path) return NULL;
  snprintf(path, len + extra, "hosts/%s.conf", address);
  return path;
}
