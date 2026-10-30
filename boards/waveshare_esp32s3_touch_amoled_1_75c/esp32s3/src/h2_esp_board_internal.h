#ifndef H2_ESP_BOARD_INTERNAL_H
#define H2_ESP_BOARD_INTERNAL_H

#include "driver/i2c_master.h"
#include "h2_pal.h"

i2c_master_bus_handle_t h2_ws175c_board_i2c_bus(void);
h2_pal_result_t h2_ws175c_board_audio_power_on(void);
h2_pal_result_t h2_ws175c_board_power_button_read(int* out_pressed);
int h2_esp_board_display_power_off(void);

#endif
