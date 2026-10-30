#include "h2_esp_p4_board_internal.h"

#include <stdatomic.h>
#include <stddef.h>

static i2c_master_bus_handle_t s_bus;
static atomic_flag s_operation = ATOMIC_FLAG_INIT;

i2c_master_bus_handle_t h2_esp_p4_board_i2c_bus(void) {
  if (atomic_flag_test_and_set_explicit(&s_operation, memory_order_acquire))
    return NULL;
  if (s_bus == NULL) {
    const i2c_master_bus_config_t config = {
        .i2c_port = 0,
        .sda_io_num = 7,
        .scl_io_num = 8,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    if (i2c_new_master_bus(&config, &s_bus) != ESP_OK) s_bus = NULL;
  }
  i2c_master_bus_handle_t bus = s_bus;
  atomic_flag_clear_explicit(&s_operation, memory_order_release);
  return bus;
}

h2_pal_result_t h2_esp_p4_board_i2c_deinit(void) {
  if (atomic_flag_test_and_set_explicit(&s_operation, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  esp_err_t err = s_bus == NULL ? ESP_OK : i2c_del_master_bus(s_bus);
  if (err == ESP_OK) s_bus = NULL;
  atomic_flag_clear_explicit(&s_operation, memory_order_release);
  return err == ESP_OK ? H2_PAL_OK : H2_PAL_ERR_IO;
}
