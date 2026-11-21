#ifndef H2_ESP_BOARD_PRIVATE_H
#define H2_ESP_BOARD_PRIVATE_H
#include "h2_esp_board.h"
#include "h2_esp_platform_core.h"

const h2_pal_display_api_t* h2_passport_display(void);
int h2_passport_display_deinit(void);
int h2_passport_peripherals_init(void);
int h2_passport_peripherals_deinit(void);
const h2_pal_periph_api_t* h2_passport_periph(void);
const h2_pal_button_api_t* h2_passport_button(void);
const h2_pal_input_api_t* h2_passport_input(void);
h2_pal_audio_t* h2_passport_audio(void);
#endif
