#ifndef H2_ESP_TIGA_BOARD_INTERNAL_H
#define H2_ESP_TIGA_BOARD_INTERNAL_H

#include "driver/i2c_master.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

i2c_master_bus_handle_t h2_esp_tiga_board_i2c_bus(void);
int h2_esp_tiga_board_enable_peripheral_power(void);
int h2_esp_tiga_display_validate_fixed_memory(void);
int h2_esp_tiga_board_codec_pa_set(void* user, int enabled);
int h2_esp_tiga_board_modem_atomics_prepare(void);
bool h2_esp_tiga_board_modem_call_audio_active(void);

#ifdef __cplusplus
}
#endif

#endif
