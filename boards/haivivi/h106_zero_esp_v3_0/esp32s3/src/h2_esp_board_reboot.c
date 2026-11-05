#include "h2_esp_board_config.h"
#include "h2/pal/core/h2_pal_errors.h"
#include "driver/gpio.h"

/* H2Loader can reboot from its own task while App/Runtime still own audio
 * tracks and workers. Reset reclaims them; destroying the mixer here races
 * downlink drain/close. Only mute the external PA, without SDK teardown. */
h2_pal_result_t h2_esp_platform_power_before_reboot(uint32_t reason) {
  (void)reason;
  return gpio_set_level(H2_ZERO_ESP_AUDIO_PA_GPIO, 0) == ESP_OK ? H2_PAL_OK
                                                                : H2_PAL_ERR_IO;
}
