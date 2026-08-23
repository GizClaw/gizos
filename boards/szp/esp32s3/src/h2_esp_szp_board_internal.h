#ifndef H2_ESP_SZP_BOARD_INTERNAL_H
#define H2_ESP_SZP_BOARD_INTERNAL_H

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

int h2_esp_szp_board_init_io(void);
i2c_master_bus_handle_t h2_esp_szp_board_i2c_bus(void);
int h2_esp_szp_board_set_lcd_cs(int high);
int h2_esp_szp_board_set_pa(int enabled);
int h2_esp_board_display_power_off(void);

#ifdef __cplusplus
}
#endif

#endif
