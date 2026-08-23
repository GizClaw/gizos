#ifndef H2_BK3633_INTERACTION_SDK_FAKE_H
#define H2_BK3633_INTERACTION_SDK_FAKE_H

#include "h2_bk3633_platform_core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct h2_bk3633_interaction_pwm_call {
    uint8_t block;
    uint8_t channel;
    bool initialize;
    bool continuous_mode;
    uint32_t end_value;
    uint32_t duty_cycle;
} h2_bk3633_interaction_pwm_call_t;

void h2_bk3633_interaction_sdk_fake_reset(void);
void h2_bk3633_interaction_sdk_fake_set_gpio(
    uint8_t gpio_pin,
    uint8_t level);
void h2_bk3633_interaction_sdk_fake_set_time(uint64_t now_ms);
void h2_bk3633_interaction_sdk_fake_fail_gpio_config_call(
    size_t one_based_call,
    h2_pal_result_t result);
void h2_bk3633_interaction_sdk_fake_fail_next_gpio_read(
    h2_pal_result_t result);
void h2_bk3633_interaction_sdk_fake_fail_next_time(
    h2_pal_result_t result);
void h2_bk3633_interaction_sdk_fake_fail_pwm_call(
    size_t one_based_call,
    h2_pal_result_t result);

size_t h2_bk3633_interaction_sdk_fake_gpio_release_count(void);
size_t h2_bk3633_interaction_sdk_fake_pwm_call_count(void);
const h2_bk3633_interaction_pwm_call_t *
h2_bk3633_interaction_sdk_fake_pwm_call(size_t index);
const h2_pal_time_api_t *h2_bk3633_interaction_sdk_fake_time_api(void);

/* Provider-facing SDK seams selected only by native tests. */
h2_pal_result_t h2_bk3633_interaction_sdk_gpio_config(
    uint8_t gpio_pin,
    h2_bk3633_platform_button_pull_t pull);
h2_pal_result_t h2_bk3633_interaction_sdk_gpio_read(
    uint8_t gpio_pin,
    uint8_t *out_level);
void h2_bk3633_interaction_sdk_gpio_release(uint8_t gpio_pin);
h2_pal_result_t h2_bk3633_interaction_sdk_pwm_initialize(
    uint8_t block,
    uint8_t channel,
    bool continuous_mode,
    uint32_t end_value,
    uint32_t duty_cycle);
h2_pal_result_t h2_bk3633_interaction_sdk_pwm_update(
    uint8_t block,
    uint8_t channel,
    uint32_t end_value,
    uint32_t duty_cycle);

#endif
