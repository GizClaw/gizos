#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_protocol.h"
#include "h2_esp_board.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint32_t build_timestamp(void) {
  struct tm value = {0};
  char month[4];
  int day, year, hour, minute, second;
  if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3 ||
      sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3)
    return 0u;
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* start = strstr(months, month);
  if (start == NULL) return 0u;
  value.tm_year = year - 1900;
  value.tm_mon = (int)(start - months) / 3;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  return (uint32_t)timegm(&value);
}

uint32_t h2_alipay_e2e_target_feature(void) { return 16385u; }

bool h2_alipay_e2e_target_complete_binding(void) { return false; }

h2_pal_result_t h2_alipay_e2e_target_configure(h2_runtime_t* runtime) {
  return h2_alipay_tiga_protocol_configure(runtime, build_timestamp());
}

h2_display_info_t h2_alipay_e2e_target_capture(h2_alipay_e2e_capture_fn capture,
                                               void* user) {
  return h2_esp_board_display_set_transfer_capture(capture, user);
}
