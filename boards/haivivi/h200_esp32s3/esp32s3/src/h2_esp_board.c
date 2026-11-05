#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"
#include "h2_esp_h200_board_internal.h"
#include "h2_esp_platform_core.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char* TAG = "h2_esp_h200";
static i2c_master_bus_handle_t s_i2c_bus;
static int s_io_initialized;

static int set_output_level(int gpio, int enabled) {
  return gpio_set_level((gpio_num_t)gpio, enabled ? 1 : 0) == ESP_OK
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}

int h2_esp_h200_board_init_io(void) {
  if (s_io_initialized) {
    return H2_PAL_OK;
  }
  const gpio_config_t output_config = {
      .pin_bit_mask =
          (1ULL << H2_H200_CODEC_POWER_GPIO) | (1ULL << H2_H200_AUDIO_PA_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&output_config) != ESP_OK ||
      set_output_level(H2_H200_AUDIO_PA_GPIO, 0) != H2_PAL_OK ||
      set_output_level(H2_H200_CODEC_POWER_GPIO, 1) != H2_PAL_OK) {
    return H2_PAL_ERR_IO;
  }

  const i2c_master_bus_config_t bus_config = {
      .i2c_port = H2_H200_I2C_PORT,
      .sda_io_num = H2_H200_I2C_SDA_GPIO,
      .scl_io_num = H2_H200_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags = {.enable_internal_pullup = true},
  };
  esp_err_t err = i2c_new_master_bus(&bus_config, &s_i2c_bus);
  if (err == ESP_ERR_INVALID_STATE) {
    err = i2c_master_get_bus_handle(H2_H200_I2C_PORT, &s_i2c_bus);
  }
  if (err != ESP_OK) {
    (void)set_output_level(H2_H200_CODEC_POWER_GPIO, 0);
    ESP_LOGE(TAG, "i2c init failed: %s", esp_err_to_name(err));
    return H2_PAL_ERR_IO;
  }
  s_io_initialized = 1;
  return H2_PAL_OK;
}

i2c_master_bus_handle_t h2_esp_h200_board_i2c_bus(void) {
  return h2_esp_h200_board_init_io() == H2_PAL_OK ? s_i2c_bus : NULL;
}

int h2_esp_h200_board_set_codec_power(int enabled) {
  return set_output_level(H2_H200_CODEC_POWER_GPIO, enabled);
}

int h2_esp_h200_board_set_pa(int enabled) {
  return set_output_level(H2_H200_AUDIO_PA_GPIO, enabled);
}

int h2_esp_board_h2loader_fs_init(h2_pal_fs_api_t* fs);
int h2_esp_board_h2loader_fs_deinit(void);

h2_pal_mem_api_t* h2_esp_board_default_allocator(void) {
  return h2_esp_platform_default_allocator();
}
h2_pal_mem_api_t* h2_esp_board_psram_allocator(void) {
  return h2_esp_platform_psram_allocator();
}
h2_pal_mem_api_t* h2_esp_board_internal_allocator(void) {
  return h2_esp_platform_internal_allocator();
}
h2_pal_mem_api_t* h2_esp_board_dma_allocator(void) {
  return h2_esp_platform_dma_allocator();
}
const h2_pal_log_api_t* h2_esp_board_log_api(void) {
  return h2_esp_platform_log_api();
}
const h2_pal_sync_api_t* h2_esp_board_sync_api(void) {
  return h2_esp_platform_sync_api();
}
const h2_pal_task_api_t* h2_esp_board_task_api(void) {
  return h2_esp_platform_task_api();
}
const h2_pal_queue_api_t* h2_esp_board_queue_api(void) {
  return h2_esp_platform_queue_api();
}
const h2_pal_time_api_t* h2_esp_board_time_api(void) {
  return h2_esp_platform_time_api();
}
const h2_pal_system_event_api_t* h2_esp_board_system_event_api(void) {
  return h2_esp_platform_system_event_api();
}
const h2_pal_crypto_api_t* h2_esp_board_crypto_api(void) {
  return h2_esp_platform_crypto_api();
}
const h2_pal_disk_api_t* h2_esp_board_disk_api(void) {
  return h2_esp_platform_disk_api();
}
const h2_pal_pref_api_t* h2_esp_board_pref_api(void) {
  return h2_esp_platform_pref_api();
}
const h2_pal_webrtc_api_t* h2_esp_board_webrtc_api(void) {
  return h2_esp_platform_webrtc_api();
}
h2_pal_ble_t* h2_esp_board_ble(void) { return h2_esp_platform_ble(); }
h2_pal_wifi_sta_t* h2_esp_board_wifi_sta(void) {
  return h2_esp_platform_wifi_sta();
}
h2_pal_wifi_ap_t* h2_esp_board_wifi_ap(void) {
  return h2_esp_platform_wifi_ap();
}
h2_pal_wifi_settings_t* h2_esp_board_wifi_settings(void) {
  return h2_esp_platform_wifi_settings();
}
h2_pal_modem_t* h2_esp_board_modem(void) {
  return h2_esp_platform_modem_unsupported();
}
