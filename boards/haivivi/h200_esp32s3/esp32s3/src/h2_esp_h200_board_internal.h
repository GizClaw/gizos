#ifndef H2_ESP_H200_BOARD_INTERNAL_H
#define H2_ESP_H200_BOARD_INTERNAL_H

#include "driver/i2c_master.h"

/* Recorded hardware facts that are not published as supported PAL capabilities.
 */
#define H2_H200_MOTION_I2C_ADDRESS 0x25u
#define H2_H200_MOTION_IRQ_GPIO 21
#define H2_H200_VIBRATION_GPIO 45
#define H2_H200_MODEM_ENABLE_GPIO 42
#define H2_H200_MODEM_UART_TX_GPIO 17
#define H2_H200_MODEM_UART_RX_GPIO 18

int h2_esp_h200_board_init_io(void);
i2c_master_bus_handle_t h2_esp_h200_board_i2c_bus(void);
int h2_esp_h200_board_set_codec_power(int enabled);
int h2_esp_h200_board_set_pa(int enabled);

#endif
