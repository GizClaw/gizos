#ifndef H2_BK7258_DISPLAY_BACKLIGHT_TEST_SDK_H
#define H2_BK7258_DISPLAY_BACKLIGHT_TEST_SDK_H
#include <stdint.h>
typedef int bk_err_t;
typedef int pwm_chan_t;
#define BK_OK 0
#define PWM_CH_1 1
typedef struct pwm_init_config {
    uint32_t period_cycle;
    uint32_t duty_cycle;
} pwm_init_config_t;
typedef struct pwm_period_duty_config {
    uint32_t period_cycle;
    uint32_t duty_cycle;
} pwm_period_duty_config_t;
bk_err_t bk_pwm_driver_init(void);
bk_err_t bk_pwm_init(pwm_chan_t channel, const pwm_init_config_t *config);
bk_err_t bk_pwm_start(pwm_chan_t channel);
bk_err_t bk_pwm_deinit(pwm_chan_t channel);
bk_err_t bk_pwm_set_period_duty(pwm_chan_t channel, const pwm_period_duty_config_t *config);
#endif
