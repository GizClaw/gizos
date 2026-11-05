#include "h2_esp_board.h"
#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"
#include "h2_esp_platform_arena.h"
#include "h2_esp_tiga_board_internal.h"
#include "h2_esp_platform_core.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#define H2_TIGA_ESP_I2C_PORT 0
#define H2_TIGA_ESP_I2C_SDA_GPIO 2
#define H2_TIGA_ESP_I2C_SCL_GPIO 1

static const char* TAG = "h2_esp_tiga";
static i2c_master_bus_handle_t s_i2c_bus;

i2c_master_bus_handle_t h2_esp_tiga_board_i2c_bus(void) {
  if (s_i2c_bus != NULL) {
    return s_i2c_bus;
  }

  const gpio_config_t gpio_cfg = {
      .pin_bit_mask = (1ULL << H2_TIGA_ESP_I2C_SDA_GPIO) |
                      (1ULL << H2_TIGA_ESP_I2C_SCL_GPIO),
      .mode = GPIO_MODE_DISABLE,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&gpio_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c gpio reset failed: %s", esp_err_to_name(err));
    return NULL;
  }

  const i2c_master_bus_config_t bus_cfg = {
      .i2c_port = H2_TIGA_ESP_I2C_PORT,
      .sda_io_num = H2_TIGA_ESP_I2C_SDA_GPIO,
      .scl_io_num = H2_TIGA_ESP_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags = {.enable_internal_pullup = true},
  };
  err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
  if (err == ESP_ERR_INVALID_STATE) {
    err = i2c_master_get_bus_handle(H2_TIGA_ESP_I2C_PORT, &s_i2c_bus);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c init failed: %s", esp_err_to_name(err));
    s_i2c_bus = NULL;
  }
  return s_i2c_bus;
}

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

/* Kept for the image lifetime: Runtime may restart while borrowers retain the
 * API. Bring-up calls are serialized before the entry task starts services. */
static h2_esp_platform_arena_t* s_arena;
static bool s_arena_attempted;

void h2_esp_board_arena_init(void) {
  if (s_arena_attempted) return;
  s_arena_attempted = true;
  const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const size_t reserved =
      h2_esp_board_arena_reserved_bytes(free_bytes, largest);
  const h2_esp_platform_arena_config_t config = {
      .reserved_bytes = reserved,
      .small_request_max = H2_TIGA_ESP_ARENA_SMALL_REQUEST_MAX,
      .small_pool_bytes = H2_TIGA_ESP_ARENA_SMALL_POOL_BYTES,
      .name = "tiga_esp_v4_2",
  };
  const h2_pal_result_t rc = h2_esp_platform_arena_create(&config, &s_arena);
  if (rc != H2_PAL_OK)
    ESP_LOGW(TAG, "arena unavailable requested=%zu rc=%d",
             config.reserved_bytes, rc);
}

const h2_pal_mem_api_t* h2_esp_board_arena_mem(void) {
  return h2_esp_platform_arena_mem(s_arena);
}
