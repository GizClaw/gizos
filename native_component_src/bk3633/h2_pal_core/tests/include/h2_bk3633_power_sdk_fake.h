#ifndef H2_BK3633_POWER_SDK_FAKE_H
#define H2_BK3633_POWER_SDK_FAKE_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stdint.h>

typedef struct rwip_time {
    uint32_t hs;
    uint16_t hus;
} rwip_time_t;

rwip_time_t rwip_time_get(void);
void h2_bk3633_power_sdk_fake_set_time(uint32_t half_slots,
                                       uint16_t half_microseconds);

void h2_bk3633_power_sdk_fake_reset(void);
void h2_bk3633_power_sdk_fake_set_gpio(uint8_t pin, uint8_t level);
void h2_bk3633_power_sdk_fake_set_adc_ready(uint16_t raw);
void h2_bk3633_power_sdk_fake_set_adc_result(h2_pal_result_t result);
unsigned int h2_bk3633_power_sdk_fake_adc_start_count(void);
unsigned int h2_bk3633_power_sdk_fake_adc_abort_count(void);
unsigned int h2_bk3633_power_sdk_fake_adc_init_count(void);
uint8_t h2_bk3633_power_sdk_fake_adc_init_channel(void);
uint8_t h2_bk3633_power_sdk_fake_adc_init_mode(void);
unsigned int h2_bk3633_power_sdk_fake_adc_deinit_count(void);
uint8_t h2_bk3633_power_sdk_fake_last_output_low_pin(void);
uint8_t h2_bk3633_power_sdk_fake_last_output_high_pin(void);
void h2_bk3633_power_sdk_fake_set_reset_reason(uint8_t reason);
void h2_bk3633_power_sdk_fake_set_rwip_sleep(uint8_t result);
unsigned int h2_bk3633_power_sdk_fake_cpu_sleep_count(void);
unsigned int h2_bk3633_power_sdk_fake_cpu_wakeup_count(void);
unsigned int h2_bk3633_power_sdk_fake_deep_sleep_count(void);
unsigned int h2_bk3633_power_sdk_fake_reboot_count(void);
uint32_t h2_bk3633_power_sdk_fake_reboot_reason(void);
uint8_t h2_bk3633_power_sdk_fake_deep_sleep_wake_pin(void);

void h2_bk3633_power_sdk_fake_adc_init(uint8_t channel, uint8_t mode);
h2_pal_result_t h2_bk3633_power_sdk_fake_adc_start(uint8_t channel,
                                                   uint8_t mode);
h2_pal_result_t h2_bk3633_power_sdk_fake_adc_take(uint16_t *out_raw);
void h2_bk3633_power_sdk_fake_adc_abort(uint8_t channel);
void h2_bk3633_power_sdk_fake_adc_deinit(uint8_t channel);
uint8_t h2_bk3633_power_sdk_fake_gpio_read(uint8_t pin);
void h2_bk3633_power_sdk_fake_gpio_input_pull_up(uint8_t pin);
void h2_bk3633_power_sdk_fake_gpio_output_low(uint8_t pin);
void h2_bk3633_power_sdk_fake_gpio_output_high(uint8_t pin);
uint8_t h2_bk3633_power_sdk_fake_reset_reason(void);
uint8_t h2_bk3633_power_sdk_fake_rwip_sleep(void);
void h2_bk3633_power_sdk_fake_set_sleep_mode(uint8_t mode);
void h2_bk3633_power_sdk_fake_cpu_sleep(void);
void h2_bk3633_power_sdk_fake_cpu_wakeup(void);
void h2_bk3633_power_sdk_fake_set_deep_sleep_wake(uint8_t pin);
void h2_bk3633_power_sdk_fake_deep_sleep(void);
void h2_bk3633_power_sdk_fake_set_reboot_reason(uint32_t reason);
void h2_bk3633_power_sdk_fake_reboot(void);

#define RWIP_MAX_CLOCK_TIME 0x0fffffffu

#endif
