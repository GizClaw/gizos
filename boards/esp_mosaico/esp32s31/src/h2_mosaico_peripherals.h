#ifndef H2_MOSAICO_PERIPHERALS_H
#define H2_MOSAICO_PERIPHERALS_H
#include "h2_pal.h"
int h2_mosaico_peripherals_init(void);
int h2_mosaico_peripherals_deinit(void);
const h2_pal_periph_api_t *h2_mosaico_periph(void);
const h2_pal_button_api_t *h2_mosaico_button(void);
const h2_pal_imu_api_t *h2_mosaico_imu(void);
const h2_pal_input_api_t *h2_mosaico_input(void);
const h2_pal_pwm_switch_api_t *h2_mosaico_motor(void);
#endif
