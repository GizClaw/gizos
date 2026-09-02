#include "h2_esp_board_internal.h"
#include "h2_esp_board_private.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H2_FT3168_I2C_ADDRESS 0x38u
#define H2_FT3168_I2C_SPEED_HZ 400000u
#define H2_FT3168_TIMEOUT_MS 20
#define H2_FT3168_REG_TOUCH_COUNT 0x02u
#define H2_FT3168_REG_DEVICE_ID 0xa0u
#define H2_FT3168_REG_POWER_MODE 0xa5u
#define H2_FT3168_DEVICE_ID 0x03u
#define H2_FT3168_POWER_ACTIVE 0x00u
#define H2_FT3168_WIDTH 368
#define H2_FT3168_HEIGHT 448
#define H2_FT3168_INTERRUPT_GPIO GPIO_NUM_21
#define H2_FT3168_RESET_MASK (1u << 2)

typedef struct h2_esp_ft3168_state {
  i2c_master_dev_handle_t device;
  int32_t last_x;
  int32_t last_y;
  bool opened;
  bool contact_active;
} h2_esp_ft3168_state_t;

static const char *TAG = "h2_ft3168";
static h2_esp_ft3168_state_t s_touch;

static h2_pal_result_t map_esp_err(esp_err_t err) {
  if (err == ESP_OK) {
    return H2_PAL_OK;
  }
  if (err == ESP_ERR_INVALID_ARG) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (err == ESP_ERR_NO_MEM) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  if (err == ESP_ERR_TIMEOUT) {
    return H2_PAL_ERR_TIMEOUT;
  }
  if (err == ESP_ERR_NOT_FOUND) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  return H2_PAL_ERR_IO;
}

static esp_err_t read_registers(uint8_t reg, uint8_t *out, size_t len) {
  esp_err_t err = ESP_FAIL;
  for (unsigned attempt = 0u; attempt < 3u; ++attempt) {
    err = i2c_master_transmit_receive(s_touch.device, &reg, sizeof(reg), out,
                                      len, H2_FT3168_TIMEOUT_MS);
    if (err == ESP_OK) {
      break;
    }
    if (attempt + 1u < 3u) {
      vTaskDelay(pdMS_TO_TICKS(1u));
    }
  }
  return err;
}

static esp_err_t write_register(uint8_t reg, uint8_t value) {
  const uint8_t data[] = {reg, value};
  return i2c_master_transmit(s_touch.device, data, sizeof(data),
                             H2_FT3168_TIMEOUT_MS);
}

static esp_err_t reset_touch(void) {
  esp_err_t err =
      h2_esp_amoled_board_io_update_outputs(H2_FT3168_RESET_MASK, 0u);
  if (err != ESP_OK) {
    return err;
  }
  vTaskDelay(pdMS_TO_TICKS(20u));
  err = h2_esp_amoled_board_io_update_outputs(H2_FT3168_RESET_MASK,
                                              H2_FT3168_RESET_MASK);
  if (err == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(100u));
  }
  return err;
}

static h2_pal_result_t touch_open(void *user) {
  (void)user;
  if (s_touch.opened) {
    return H2_PAL_OK;
  }

  i2c_master_bus_handle_t bus = h2_esp_amoled_board_i2c_bus();
  if (bus == NULL) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  const gpio_config_t interrupt_config = {
      .pin_bit_mask = 1ULL << H2_FT3168_INTERRUPT_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&interrupt_config);
  if (err == ESP_OK) {
    err = reset_touch();
  }
  if (err == ESP_OK) {
    err = i2c_master_probe(bus, H2_FT3168_I2C_ADDRESS, H2_FT3168_TIMEOUT_MS);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "FT3168 probe at 0x38 failed: %s", esp_err_to_name(err));
    (void)h2_esp_amoled_board_io_update_outputs(H2_FT3168_RESET_MASK, 0u);
    (void)gpio_reset_pin(H2_FT3168_INTERRUPT_GPIO);
    return map_esp_err(err);
  }

  const i2c_device_config_t device_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = H2_FT3168_I2C_ADDRESS,
      .scl_speed_hz = H2_FT3168_I2C_SPEED_HZ,
  };
  err = i2c_master_bus_add_device(bus, &device_config, &s_touch.device);
  if (err != ESP_OK) {
    s_touch.device = NULL;
    (void)h2_esp_amoled_board_io_update_outputs(H2_FT3168_RESET_MASK, 0u);
    (void)gpio_reset_pin(H2_FT3168_INTERRUPT_GPIO);
    return map_esp_err(err);
  }

  uint8_t device_id = 0u;
  err = read_registers(H2_FT3168_REG_DEVICE_ID, &device_id, sizeof(device_id));
  if (err == ESP_OK && device_id != H2_FT3168_DEVICE_ID) {
    ESP_LOGE(TAG, "unexpected FT3168 device id: 0x%02x", device_id);
    err = ESP_ERR_NOT_FOUND;
  }
  if (err == ESP_OK) {
    err = write_register(H2_FT3168_REG_POWER_MODE, H2_FT3168_POWER_ACTIVE);
  }
  if (err != ESP_OK) {
    (void)i2c_master_bus_rm_device(s_touch.device);
    s_touch.device = NULL;
    (void)h2_esp_amoled_board_io_update_outputs(H2_FT3168_RESET_MASK, 0u);
    (void)gpio_reset_pin(H2_FT3168_INTERRUPT_GPIO);
    return map_esp_err(err);
  }

  s_touch.opened = true;
  s_touch.contact_active = false;
  ESP_LOGI(TAG, "FT3168 ready address=0x38 id=0x%02x", device_id);
  return H2_PAL_OK;
}

static h2_pal_result_t touch_get_info(void *user,
                                      h2_pal_touch_info_t *out_info) {
  (void)user;
  if (out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!s_touch.opened) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  *out_info = (h2_pal_touch_info_t){
      .width = H2_FT3168_WIDTH,
      .height = H2_FT3168_HEIGHT,
  };
  return H2_PAL_OK;
}

static int32_t clamp_coordinate(int32_t coordinate, int32_t extent) {
  if (coordinate < 0) {
    return 0;
  }
  if (coordinate >= extent) {
    return extent - 1;
  }
  return coordinate;
}

static h2_pal_result_t touch_poll_event(void *user,
                                        h2_pal_touch_event_t *out_event) {
  (void)user;
  if (out_event == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!s_touch.opened) {
    return H2_PAL_ERR_INVALID_STATE;
  }

  uint8_t data[5] = {0};
  esp_err_t err = read_registers(H2_FT3168_REG_TOUCH_COUNT, data, sizeof(data));
  if (err != ESP_OK) {
    return map_esp_err(err);
  }
  const uint8_t contact_count = data[0] & 0x0fu;
  if (contact_count == 0u) {
    if (!s_touch.contact_active) {
      return H2_PAL_ERR_WOULD_BLOCK;
    }
    s_touch.contact_active = false;
    *out_event = (h2_pal_touch_event_t){
        .kind = H2_PAL_TOUCH_EVENT_UP,
        .x = s_touch.last_x,
        .y = s_touch.last_y,
    };
    return H2_PAL_OK;
  }

  const int32_t x = clamp_coordinate(
      ((int32_t)(data[1] & 0x0fu) << 8) | data[2], H2_FT3168_WIDTH);
  const int32_t y = clamp_coordinate(
      ((int32_t)(data[3] & 0x0fu) << 8) | data[4], H2_FT3168_HEIGHT);
  if (s_touch.contact_active && x == s_touch.last_x && y == s_touch.last_y) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  const h2_pal_touch_event_kind_t kind = s_touch.contact_active
                                             ? H2_PAL_TOUCH_EVENT_MOVE
                                             : H2_PAL_TOUCH_EVENT_DOWN;
  s_touch.contact_active = true;
  s_touch.last_x = x;
  s_touch.last_y = y;
  *out_event = (h2_pal_touch_event_t){
      .kind = kind,
      .x = x,
      .y = y,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t touch_close(void *user) {
  (void)user;
  if (!s_touch.opened) {
    return H2_PAL_OK;
  }
  esp_err_t err =
      h2_esp_amoled_board_io_update_outputs(H2_FT3168_RESET_MASK, 0u);
  esp_err_t remove_err = i2c_master_bus_rm_device(s_touch.device);
  if (err == ESP_OK) {
    err = remove_err;
  }
  esp_err_t gpio_err = gpio_reset_pin(H2_FT3168_INTERRUPT_GPIO);
  if (err == ESP_OK) {
    err = gpio_err;
  }
  s_touch.device = NULL;
  s_touch.opened = false;
  s_touch.contact_active = false;
  return map_esp_err(err);
}

const h2_pal_touch_api_t *h2_esp_board_touch_api(void) {
  static const h2_pal_touch_vtable_t vtable = {
      .open = touch_open,
      .get_info = touch_get_info,
      .poll_event = touch_poll_event,
      .close = touch_close,
  };
  static const h2_pal_touch_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}
