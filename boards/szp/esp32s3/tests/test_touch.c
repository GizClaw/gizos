#include "../src/h2_esp_board_private.h"
#include "esp_lcd_touch_ft5x06.h"
#include <assert.h>

static int bus, io, handle;
static int bus_available = 1;
static esp_err_t probe_result, create_io_result, create_touch_result;
static esp_err_t read_result, data_result, delete_touch_result,
    delete_io_result;
static unsigned io_creates, io_deletes, touch_creates, touch_deletes;
static uint8_t contacts;
static esp_lcd_touch_point_data_t point;

i2c_master_bus_handle_t h2_esp_szp_board_i2c_bus(void) {
  return bus_available ? &bus : NULL;
}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t arg, uint16_t address,
                           int timeout) {
  assert(arg == &bus && address == 0x38 && timeout > 0);
  return probe_result;
}
esp_err_t esp_lcd_new_panel_io_i2c(i2c_master_bus_handle_t arg,
                                   const esp_lcd_panel_io_i2c_config_t* config,
                                   esp_lcd_panel_io_handle_t* out) {
  assert(arg == &bus && config->scl_speed_hz == 100000);
  if (create_io_result != ESP_OK) return create_io_result;
  ++io_creates;
  *out = &io;
  return ESP_OK;
}
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t arg) {
  assert(arg == &io);
  if (delete_io_result != ESP_OK) return delete_io_result;
  ++io_deletes;
  return ESP_OK;
}
esp_err_t esp_lcd_touch_new_i2c_ft5x06(esp_lcd_panel_io_handle_t arg,
                                       const esp_lcd_touch_config_t* config,
                                       esp_lcd_touch_handle_t* out) {
  assert(arg == &io);
  assert(config->x_max == 240 && config->y_max == 320);
  assert(config->flags.swap_xy && config->flags.mirror_x &&
         !config->flags.mirror_y);
  assert(config->rst_gpio_num == GPIO_NUM_NC &&
         config->int_gpio_num == GPIO_NUM_NC);
  if (create_touch_result != ESP_OK) return create_touch_result;
  ++touch_creates;
  *out = &handle;
  return ESP_OK;
}
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t arg) {
  assert(arg == &handle);
  return read_result;
}
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t arg,
                                 esp_lcd_touch_point_data_t* out,
                                 uint8_t* count, uint8_t max_points) {
  assert(arg == &handle && max_points == 1);
  if (data_result != ESP_OK) return data_result;
  *out = point;
  *count = contacts;
  return ESP_OK;
}
esp_err_t esp_lcd_touch_del(esp_lcd_touch_handle_t arg) {
  assert(arg == &handle);
  if (delete_touch_result != ESP_OK) return delete_touch_result;
  ++touch_deletes;
  return ESP_OK;
}

int main(void) {
  const h2_pal_touch_api_t* api = h2_esp_board_touch_api();
  h2_pal_touch_event_t event;
  h2_pal_touch_info_t info;
  assert(h2_pal_touch_get_info(api, &info) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_pal_touch_close(api) == 0);
  bus_available = 0;
  assert(h2_pal_touch_open(api) == H2_PAL_ERR_UNAVAILABLE);
  bus_available = 1;
  probe_result = ESP_ERR_NOT_FOUND;
  assert(h2_pal_touch_open(api) == H2_PAL_ERR_NOT_FOUND);
  probe_result = ESP_OK;
  create_io_result = ESP_ERR_NO_MEM;
  assert(h2_pal_touch_open(api) == H2_PAL_ERR_NO_MEMORY);
  create_io_result = ESP_OK;
  create_touch_result = ESP_ERR_TIMEOUT;
  delete_io_result = ESP_FAIL;
  assert(h2_pal_touch_open(api) == H2_PAL_ERR_TIMEOUT);
  assert(h2_pal_touch_close(api) == H2_PAL_ERR_IO);
  delete_io_result = ESP_OK;
  assert(h2_pal_touch_close(api) == 0);
  assert(io_creates == io_deletes);
  create_touch_result = ESP_OK;
  assert(h2_pal_touch_open(api) == 0);
  assert(h2_pal_touch_open(api) == 0 && touch_creates == 1);
  assert(h2_pal_touch_get_info(api, &info) == 0);
  assert(info.width == 320 && info.height == 240);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_WOULD_BLOCK);
  contacts = 1;
  point = (esp_lcd_touch_point_data_t){.x = 90, .y = 120};
  assert(h2_pal_touch_poll_event(api, &event) == 0);
  assert(event.kind == H2_PAL_TOUCH_EVENT_DOWN && event.x == 90 &&
         event.y == 120);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_WOULD_BLOCK);
  read_result = ESP_ERR_INVALID_RESPONSE;
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_WOULD_BLOCK);
  read_result = ESP_ERR_TIMEOUT;
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_TIMEOUT);
  read_result = ESP_OK;
  data_result = ESP_FAIL;
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_IO);
  data_result = ESP_OK;
  point.x = 110;
  assert(h2_pal_touch_poll_event(api, &event) == 0);
  assert(event.kind == H2_PAL_TOUCH_EVENT_MOVE);
  point = (esp_lcd_touch_point_data_t){.x = 320, .y = 240};
  assert(h2_pal_touch_poll_event(api, &event) == 0);
  assert(event.x == 319 && event.y == 239);
  contacts = 0;
  assert(h2_pal_touch_poll_event(api, &event) == 0);
  assert(event.kind == H2_PAL_TOUCH_EVENT_UP && event.x == 319 &&
         event.y == 239);
  assert(h2_pal_touch_poll_event(api, &event) == H2_PAL_ERR_WOULD_BLOCK);
  delete_touch_result = ESP_FAIL;
  assert(h2_pal_touch_close(api) == H2_PAL_ERR_IO);
  assert(h2_pal_touch_get_info(api, &info) == 0);
  delete_touch_result = ESP_OK;
  delete_io_result = ESP_FAIL;
  assert(h2_pal_touch_close(api) == H2_PAL_ERR_IO && touch_deletes == 1);
  assert(h2_pal_touch_get_info(api, &info) == H2_PAL_ERR_INVALID_STATE);
  delete_io_result = ESP_OK;
  assert(h2_pal_touch_close(api) == 0 && touch_deletes == 1);
  assert(h2_pal_touch_close(api) == 0);
  assert(io_creates == io_deletes && touch_creates == touch_deletes);
  assert(h2_pal_touch_open(api) == 0);
  assert(h2_pal_touch_close(api) == 0);
  assert(io_creates == io_deletes && touch_creates == touch_deletes);
  return 0;
}
