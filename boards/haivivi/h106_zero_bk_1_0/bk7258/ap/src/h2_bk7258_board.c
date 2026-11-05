#include "h2_bk7258_board.h"
#include "h2_bk7258_board_private.h"

#include "driver/gpio.h"
#include "gpio_driver.h"

#include <stdio.h>
#include <string.h>

const h2_bk7258_audio_config_t h2_bk7258_audio_config = {
    .sample_rate = 16000u,
    .frame_samples_per_channel = 320u,
    .channels = 1u,
    .mic_channels = 1u,
    .speaker_channels = 1u,
    .bits_per_sample = 16u,
    .default_volume = 0x28u,
    .default_mic_gain = 0x2au,
    .mic_gain_min = 0x00u,
    /* Nine ADC gain steps above the existing full-duplex baseline. */
    .mic_gain_max = 0x33u,
    .frame_count = 4u,
};

const h2_bk7258_button_adc_range_t h2_bk7258_button_ranges[] = {
    {.id = 102u, .min_mv = 1u, .max_mv = 100u},
    {.id = 101u, .min_mv = 800u, .max_mv = 930u},
    {.id = 104u, .min_mv = 1600u, .max_mv = 1800u},
};

const char* h2_bk7258_board_ble_local_name(void) {
  static char name[17];
  if (name[0] != '\0') return name;
  uint8_t mac[6];
  static const uint8_t no_mac[6] = {0};
  if (h2_pal_wifi_sta_get_mac(h2_bk7258_board_wifi_sta(), mac) != H2_PAL_OK ||
      (mac[0] & 1u) != 0u || memcmp(mac, no_mac, sizeof(mac)) == 0) {
    (void)h2_pal_log_write(h2_bk7258_board_log_api(), H2_PAL_LOG_WARN,
                           "h106-zero/ble", "device MAC unavailable");
    return NULL;
  }
  const int length =
      snprintf(name, sizeof(name), "h106-zero-%02X%02X%02X", (unsigned)mac[3],
               (unsigned)mac[4], (unsigned)mac[5]);
  if (length != 16) {
    name[0] = '\0';
    return NULL;
  }
  return name;
}

const size_t h2_bk7258_button_range_count =
    sizeof(h2_bk7258_button_ranges) / sizeof(h2_bk7258_button_ranges[0]);

void h2_bk7258_board_audio_power_set(int enabled) {
  static const gpio_id_t pins[] = {GPIO_5, GPIO_12, GPIO_13};
  gpio_config_t config = {
      .io_mode = GPIO_OUTPUT_ENABLE,
      .pull_mode = GPIO_PULL_DISABLE,
      .func_mode = GPIO_SECOND_FUNC_DISABLE,
  };

  for (size_t i = 0u; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    (void)gpio_dev_unmap(pins[i]);
    (void)bk_gpio_set_config(pins[i], &config);
  }
  (void)bk_gpio_set_output_low(GPIO_5);
  (void)bk_gpio_set_output_high(GPIO_13);
  if (enabled) {
    (void)bk_gpio_set_output_high(GPIO_12);
  } else {
    (void)bk_gpio_set_output_low(GPIO_12);
  }
}
