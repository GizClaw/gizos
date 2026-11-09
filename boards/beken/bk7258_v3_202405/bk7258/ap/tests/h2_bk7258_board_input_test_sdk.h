#ifndef H2_BK7258_BOARD_INPUT_TEST_SDK_H
#define H2_BK7258_BOARD_INPUT_TEST_SDK_H

#include <stdint.h>

typedef int bk_err_t;
typedef int adc_chan_t;
typedef int gpio_id_t;
typedef int gpio_dev_t;
enum {
  BK_OK = 0,
  BK_FAIL = -1,
  BK_ERR_NULL_PARAM = -2,
  BK_ERR_PARAM = -3,
  GPIO_12 = 12,
  GPIO_DEV_ADC14 = 114,
  ADC_CONTINUOUS_MODE = 3,
  ADC_SCLK_XTAL_26M = 1,
  ADC_SATURATE_MODE_3 = 4,
};
typedef struct {
  adc_chan_t chan;
  int adc_mode;
  int src_clk;
  uint32_t clk;
  int saturate_mode;
  uint32_t steady_ctrl;
  uint32_t adc_filter;
  uint32_t sample_rate;
} adc_config_t;

bk_err_t gpio_dev_unmap(gpio_id_t gpio);
bk_err_t gpio_dev_map(gpio_id_t gpio, gpio_dev_t dev);
bk_err_t rtos_delay_milliseconds(uint32_t ms);
bk_err_t bk_adc_acquire(void);
bk_err_t bk_adc_init(adc_chan_t chan);
bk_err_t bk_adc_chan_init_gpio(adc_chan_t chan);
bk_err_t bk_adc_set_config(adc_config_t* config);
bk_err_t bk_adc_enable_bypass_clalibration(void);
bk_err_t bk_adc_start(void);
bk_err_t bk_adc_set_channel(adc_chan_t chan);
bk_err_t bk_adc_read(uint16_t* raw, uint32_t timeout_ms);
bk_err_t bk_adc_stop(void);
bk_err_t bk_adc_deinit(adc_chan_t chan);
bk_err_t bk_adc_release(void);

#endif
