#include "h2_esp_board_config.h"
#include "h2_esp_board_internal.h"

#include "esp_log.h"

static i2c_master_bus_handle_t s_i2c_bus;

i2c_master_bus_handle_t h2_ws175c_board_i2c_bus(void) {
  if (s_i2c_bus != NULL) {
    return s_i2c_bus;
  }
  const i2c_master_bus_config_t config = {
      .i2c_port = H2_WS175C_I2C_PORT,
      .sda_io_num = H2_WS175C_I2C_SDA_GPIO,
      .scl_io_num = H2_WS175C_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t err = i2c_new_master_bus(&config, &s_i2c_bus);
  if (err == ESP_ERR_INVALID_STATE) {
    err = i2c_master_get_bus_handle(H2_WS175C_I2C_PORT, &s_i2c_bus);
  }
  if (err != ESP_OK) {
    ESP_LOGE("h2_ws175c_i2c", "shared I2C init failed: %s",
             esp_err_to_name(err));
    s_i2c_bus = NULL;
  }
  return s_i2c_bus;
}
