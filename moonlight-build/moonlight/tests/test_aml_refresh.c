#include "../src/video/aml_refresh.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  assert(aml_refresh_rate_x100_parse_vinfo(
      "current vinfo:\n"
      "    name:                  2160p60hz\n"
      "    sync_duration_num:     60\n"
      "    sync_duration_den:     1\n") == 6000);

  assert(aml_refresh_rate_x100_parse_vinfo(
      "current vinfo:\n"
      "    sync_duration_num:     60000\n"
      "    sync_duration_den:     1001\n") == 5994);

  assert(aml_refresh_rate_x100_parse_vinfo(
      "    sync_duration_num:     24000\n"
      "    sync_duration_den:     1001\n") == 2398);

  assert(aml_refresh_rate_x100_parse_vinfo(
      "    sync_duration_num:     50\n"
      "    sync_duration_den:     1\n") == 5000);

  assert(aml_refresh_rate_x100_parse_vinfo(
      "    sync_duration_num:     120\n"
      "    sync_duration_den:     1\n") == 12000);

  assert(aml_refresh_rate_x100_parse_vinfo(
      "    sync_duration_num:     60\n"
      "    sync_duration_den:     0\n") == 0);
  assert(aml_refresh_rate_x100_parse_vinfo("not vinfo\n") == 0);
  assert(aml_refresh_rate_x100_parse_vinfo(NULL) == 0);

  puts("Amlogic refresh-rate parser tests passed");
  return 0;
}
