#ifndef H2_ESP_P4_BOARD_INTERNAL_H
#define H2_ESP_P4_BOARD_INTERNAL_H

#include "driver/i2c_master.h"
#include "h2/pal/core/h2_pal_errors.h"

/* Audio, touch and camera borrow this board-owned bus on SDA7/SCL8. */
i2c_master_bus_handle_t h2_esp_p4_board_i2c_bus(void);
h2_pal_result_t h2_esp_p4_board_i2c_deinit(void);

#endif
