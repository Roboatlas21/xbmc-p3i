/* Regression: a 127-character hostname used to overflow main.c's 128-byte
 * stack buffer before contacting the host. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/host_config_path.h"

int main(void) {
  char host[128];
  memset(host, 'a', sizeof(host) - 1);
  host[sizeof(host) - 1] = '\0';
  char *path = moonlight_host_config_path(host);
  assert(path);
  assert(strlen(path) == strlen("hosts/") + strlen(host) + strlen(".conf"));
  assert(strncmp(path, "hosts/", strlen("hosts/")) == 0);
  assert(strncmp(path + strlen("hosts/"), host, strlen(host)) == 0);
  assert(strcmp(path + strlen("hosts/") + strlen(host), ".conf") == 0);
  free(path);
  path = moonlight_host_config_path("10.0.0.1");
  assert(path && strcmp(path, "hosts/10.0.0.1.conf") == 0);
  free(path);
  assert(moonlight_host_config_path(NULL) == NULL);
  puts("PASS: hostname config paths are fully allocated");
  return 0;
}
