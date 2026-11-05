#include "h2_esp_board_config.h"
#include "h2/pal/core/h2_pal_errors.h"
#include "driver/gpio.h"
#include <assert.h>

h2_pal_result_t h2_esp_platform_power_before_reboot(uint32_t reason);
static int calls;
static esp_err_t result;
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level) {
  assert(gpio == H2_TIGA_ESP_AUDIO_PA_GPIO && level == 0u);
  ++calls;
  return result;
}
/* Link only the GPIO stub: the asynchronous hook must never destroy audio,
 * stop consumers, or wait for any peripheral driver lock. */
int main(void) {
  assert(h2_esp_platform_power_before_reboot(0u) == H2_PAL_OK);
  assert(h2_esp_platform_power_before_reboot(1u) == H2_PAL_OK);
  result = ESP_FAIL;
  assert(h2_esp_platform_power_before_reboot(2u) == H2_PAL_ERR_IO);
  assert(calls == 3);
  return 0;
}
