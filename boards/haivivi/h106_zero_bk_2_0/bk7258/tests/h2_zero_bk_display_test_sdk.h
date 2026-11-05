#ifndef H2_ZERO_BK_DISPLAY_TEST_SDK_H
#define H2_ZERO_BK_DISPLAY_TEST_SDK_H
#include "h2_zero_bk_spi_test_sdk.h"
#define BK_OK 0
#define BK_LOGI(...) ((void)0)
#define BK_LOGE(...) ((void)0)
#define BK_LOG_ON_ERR(expr) ((void)(expr))
enum {
  GPIO_17 = 17,
  GPIO_18 = 18,
  GPIO_19 = 19,
  PWM_CH_1 = 1,
  SPI_ID_0 = 0,
  GPIO_OUTPUT_ENABLE = 1,
  GPIO_PULL_DISABLE = 0,
  GPIO_SECOND_FUNC_DISABLE = 0,
  GPIO_SPI_MAP_MODE0 = 0,
  SPI_ROLE_MASTER = 1,
  SPI_BIT_WIDTH_8BITS = 8,
  SPI_POLARITY_LOW = 0,
  SPI_PHASE_1ST_EDGE = 0,
  SPI_4WIRE_MODE = 4,
  SPI_MSB_FIRST = 0,
  SPI_DMA_MODE_ENABLE = 1,
  DMA_DATA_WIDTH_8BITS = 8
};
typedef struct {
  uint32_t io_mode, pull_mode, func_mode;
} gpio_config_t;
typedef struct {
  uint32_t period_cycle, duty_cycle, duty2_cycle, duty3_cycle, psc;
} pwm_init_config_t;
typedef pwm_init_config_t pwm_period_duty_config_t;
typedef struct {
  uint32_t role, bit_width, polarity, phase, wire_mode, baud_rate, bit_order,
      dma_mode, spi_tx_dma_width, spi_rx_dma_width;
} spi_config_t;
int gpio_dev_unmap(uint32_t pin);
int bk_gpio_set_config(uint32_t pin, const gpio_config_t* config);
int bk_gpio_set_output_high(uint32_t pin);
int bk_gpio_set_output_low(uint32_t pin);
int bk_gpio_driver_init(void);
int gpio_spi_sel(uint32_t mode);
int bk_spi_driver_init(void);
int bk_spi_init(uint32_t id, const spi_config_t* config);
int bk_spi_deinit(uint32_t id);
int bk_spi_write_bytes(uint32_t id, const void* data, uint32_t len);
int bk_pwm_driver_init(void);
int bk_pwm_init(uint32_t channel, const pwm_init_config_t* config);
int bk_pwm_start(uint32_t channel);
int bk_pwm_deinit(uint32_t channel);
int bk_pwm_set_period_duty(uint32_t channel, pwm_period_duty_config_t* config);
void rtos_delay_milliseconds(uint32_t ms);
#endif
