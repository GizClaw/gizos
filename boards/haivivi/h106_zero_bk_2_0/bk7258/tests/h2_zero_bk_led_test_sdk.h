#ifndef H2_ZERO_BK_LED_TEST_SDK_H
#define H2_ZERO_BK_LED_TEST_SDK_H

#include "h2_pal.h"
#include "h2_zero_bk_display_test_sdk.h"

#define BK_FAIL (-1)
typedef uint32_t gpio_id_t;
typedef int bk_err_t;
uint32_t bk_gpio_get_value(gpio_id_t pin);
int bk_gpio_set_value(gpio_id_t pin, uint32_t value);
const h2_pal_periph_api_t* h2_bk7258_board_periph_api(void);

#endif
