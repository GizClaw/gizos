#include "h2_esp_board_private.h"

h2_pal_result_t h2_esp_platform_power_before_reboot(uint32_t reason) {
  (void)reason;
  h2_pal_result_t rc = h2_pal_camera_stop(h2_esp_board_camera_api());
  if (rc != H2_PAL_OK) return rc;
  rc = h2_pal_touch_close(h2_esp_board_touch_api());
  if (rc != H2_PAL_OK) return rc;
  return h2_esp_board_audio_deinit();
}
