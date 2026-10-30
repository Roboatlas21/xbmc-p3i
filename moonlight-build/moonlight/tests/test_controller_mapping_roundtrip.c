/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise production parser and serializer, including no fake b0/a0 fields. */
#define _GNU_SOURCE
#include "../src/input/mapping.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  struct mapping generated = {0};
  memset(&generated.abs_leftx, 0xff,
         (char *)&generated.next - (char *)&generated.abs_leftx);
  strcpy(generated.guid, "030000005e0400008e02000014010000");
  strcpy(generated.name, "Test Controller");
  generated.abs_leftx = 1; generated.reverse_leftx = true;
  generated.abs_righty = 3; generated.reverse_righty = true;
  generated.abs_lefttrigger = 4; generated.halfaxis_lefttrigger = '+';
  generated.abs_righttrigger = 4; generated.halfaxis_righttrigger = '~';
  generated.abs_dpup = 5; generated.halfaxis_dpup = '+';
  generated.btn_a = 7;
  generated.hat_dpdown = 0; generated.hat_dir_dpdown = 0; /* Reject neutral. */

  fflush(stdout);
  /* Send production serializer output to the in-memory stream through fd. */
  int original_fd = dup(fileno(stdout));
  assert(original_fd >= 0);
  FILE *temp = tmpfile();
  assert(temp);
  assert(dup2(fileno(temp), fileno(stdout)) >= 0);
  mapping_print(&generated);
  fflush(stdout);
  assert(dup2(original_fd, fileno(stdout)) >= 0);
  close(original_fd);
  rewind(temp);
  char line[2048] = {0};
  assert(fgets(line, sizeof(line), temp));
  fclose(temp);
  assert(strstr(line, "leftx:a1~") != NULL);
  assert(strstr(line, "righty:a3~") != NULL);
  assert(strstr(line, "lefttrigger:+a4") != NULL);
  assert(strstr(line, "righttrigger:a4~") != NULL);
  assert(strstr(line, "dpup:+a5") != NULL);
  assert(strstr(line, "a:b7") != NULL);
  assert(strstr(line, "paddle1:") == NULL);
  assert(strstr(line, "touchpad:") == NULL);
  assert(strstr(line, "rightx:") == NULL);
  assert(strstr(line, "dpdown:h0.0") == NULL);
  struct mapping *parsed = mapping_parse(line);
  assert(parsed);
  assert(parsed->reverse_leftx && parsed->abs_leftx == 1);
  assert(parsed->reverse_righty && parsed->abs_righty == 3);
  assert(parsed->halfaxis_lefttrigger == '+' && parsed->abs_lefttrigger == 4);
  assert(parsed->halfaxis_righttrigger == '~' && parsed->abs_righttrigger == 4);
  assert(parsed->halfaxis_dpup == '+' && parsed->abs_dpup == 5);
  assert(parsed->btn_a == 7 && parsed->btn_paddle1 == -1);
  assert(parsed->hat_dpdown == -1);
  char invalid[] = "030000005e0400008e02000014010000,Pad,dpup:h0.0,";
  struct mapping *rejected_hat = mapping_parse(invalid);
  assert(rejected_hat && rejected_hat->hat_dpup == -1);
  free(rejected_hat);
  free(parsed);
  puts("PASS: controller mapping output preserves inversion and half axes");
  return 0;
}
