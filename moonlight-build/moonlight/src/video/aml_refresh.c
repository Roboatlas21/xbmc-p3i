/* CoreELEC Amlogic active-display refresh detection. GPL-3.0-or-later. */
#include "aml_refresh.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AML_VINFO_PATH "/sys/class/display/vinfo"
#define AML_VINFO_BUFFER_SIZE 4096

static int parse_u32_field(const char *text, const char *key, uint32_t *out) {
  if (!text || !key || !*key || !out)
    return 0;

  const size_t key_len = strlen(key);
  const char *line = text;
  while (*line) {
    const char *end = strchr(line, '\n');
    if (!end)
      end = line + strlen(line);

    const char *p = line;
    while (p < end && isspace((unsigned char)*p))
      p++;

    if ((size_t)(end - p) > key_len &&
        strncmp(p, key, key_len) == 0) {
      p += key_len;
      while (p < end && isspace((unsigned char)*p))
        p++;
      if (p >= end || *p != ':')
        return 0;
      p++;
      while (p < end && isspace((unsigned char)*p))
        p++;
      if (p >= end || !isdigit((unsigned char)*p))
        return 0;

      errno = 0;
      char *after = NULL;
      const unsigned long value = strtoul(p, &after, 10);
      if (errno || after == p || value > UINT_MAX)
        return 0;
      while (after < end && isspace((unsigned char)*after))
        after++;
      if (after != end)
        return 0;

      *out = (uint32_t)value;
      return 1;
    }

    line = *end ? end + 1 : end;
  }

  return 0;
}

int aml_refresh_rate_x100_parse_vinfo(const char *text) {
  uint32_t numerator = 0;
  uint32_t denominator = 0;

  if (!parse_u32_field(text, "sync_duration_num", &numerator) ||
      !parse_u32_field(text, "sync_duration_den", &denominator) ||
      numerator == 0 || denominator == 0)
    return 0;

  /* Report hundredths of Hz, rounded to nearest. This preserves the
   * important NTSC-family distinction: 60000/1001 -> 5994, while
   * 60/1 -> 6000. Limit to plausible HDMI display rates. */
  const uint64_t scaled = (uint64_t)numerator * 100U;
  const uint64_t refresh_x100 =
      (scaled + (uint64_t)denominator / 2U) / denominator;
  if (refresh_x100 < 1000U || refresh_x100 > 24000U)
    return 0;

  return (int)refresh_x100;
}

int aml_refresh_rate_x100(void) {
  FILE *f = fopen(AML_VINFO_PATH, "r");
  if (!f)
    return 0;

  char text[AML_VINFO_BUFFER_SIZE];
  const size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  if (n == 0)
    return 0;

  text[n] = '\0';
  return aml_refresh_rate_x100_parse_vinfo(text);
}
