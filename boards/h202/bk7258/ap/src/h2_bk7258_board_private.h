#ifndef H2_H202_BOARD_PRIVATE_H
#define H2_H202_BOARD_PRIVATE_H

#include "h2_bk7258_board.h"
#include "h2_h202_board_config.h"
#include "h2_runtime.h"

h2_pal_result_t h2_h202_input_init(void);
const h2_pal_button_api_t* h2_h202_button_api(void);
/** USB_INSERT, CHRG and DONE report ON when asserted; WHITE is on/off. */
const h2_pal_switch_api_t* h2_h202_switch_api(void);
const h2_pal_nfc_api_t* h2_h202_nfc_api(void);
const h2_pal_power_api_t* h2_h202_power_api(void);
const h2_pal_led_api_t* h2_h202_led_api(void);
h2_pal_result_t h2_h202_white_set(bool on);
bool h2_h202_white_on(void);
h2_pal_result_t h2_h202_led_deinit(void);
const h2_pal_audio_api_t* h2_h202_audio_api(void);
h2_pal_result_t h2_h202_audio_deinit(void);
h2_pal_result_t h2_h202_modem_init(void);
h2_pal_result_t h2_h202_modem_deinit(void);
const h2_pal_modem_api_t* h2_h202_modem_api(void);

#endif
