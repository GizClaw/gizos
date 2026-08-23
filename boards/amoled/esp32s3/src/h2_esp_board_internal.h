#ifndef H2_ESP_BOARD_INTERNAL_H
#define H2_ESP_BOARD_INTERNAL_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "h2_pal.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

i2c_master_bus_handle_t h2_esp_amoled_board_i2c_bus(void);
esp_err_t h2_esp_amoled_board_io_update_outputs(
    uint8_t mask,
    uint8_t value);
esp_err_t h2_esp_amoled_board_io_read_inputs(uint8_t *out_value);
h2_pal_result_t h2_esp_amoled_board_power_button_read(int *out_pressed);
int h2_esp_board_display_power_off(void);

#ifdef __cplusplus
}
#endif

#endif
