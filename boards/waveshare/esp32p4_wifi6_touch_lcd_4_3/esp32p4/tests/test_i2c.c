#include "../src/h2_esp_p4_board_internal.h"
#include <assert.h>
#include <stddef.h>

static int bus;
static unsigned creates, deletes;
static esp_err_t create_error, delete_error;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* config,
                             i2c_master_bus_handle_t* out) {
  assert(config->i2c_port == 0 && config->sda_io_num == 7 &&
         config->scl_io_num == 8 && config->glitch_ignore_cnt == 7 &&
         config->flags.enable_internal_pullup);
  assert(h2_esp_p4_board_i2c_bus() == NULL);
  assert(h2_esp_p4_board_i2c_deinit() == H2_PAL_ERR_BUSY);
  if (create_error != ESP_OK) return create_error;
  ++creates;
  *out = &bus;
  return ESP_OK;
}
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t handle) {
  assert(handle == &bus);
  if (delete_error != ESP_OK) return delete_error;
  ++deletes;
  return ESP_OK;
}
int main(void) {
  assert(h2_esp_p4_board_i2c_deinit() == 0);
  create_error = ESP_ERR_NO_MEM;
  assert(h2_esp_p4_board_i2c_bus() == NULL);
  create_error = ESP_OK;
  assert(h2_esp_p4_board_i2c_bus() == &bus && creates == 1);
  assert(h2_esp_p4_board_i2c_bus() == &bus && creates == 1);
  delete_error = ESP_ERR_INVALID_STATE;
  assert(h2_esp_p4_board_i2c_deinit() == H2_PAL_ERR_IO);
  assert(h2_esp_p4_board_i2c_bus() == &bus && creates == 1);
  delete_error = ESP_OK;
  assert(h2_esp_p4_board_i2c_deinit() == 0 && deletes == 1);
  assert(h2_esp_p4_board_i2c_deinit() == 0 && deletes == 1);
  assert(h2_esp_p4_board_i2c_bus() == &bus && creates == 2);
  assert(h2_esp_p4_board_i2c_deinit() == 0 && deletes == 2);
  return 0;
}
