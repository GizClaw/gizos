#include "h2_bk3633_power_sdk_fake.h"

#include <string.h>

static rwip_time_t s_now;
static uint8_t s_gpio[256];
static uint16_t s_adc_raw;
static h2_pal_result_t s_adc_result;
static unsigned int s_adc_start_count;
static unsigned int s_adc_abort_count;
static unsigned int s_adc_init_count;
static uint8_t s_adc_init_channel;
static uint8_t s_adc_init_mode;
static unsigned int s_adc_deinit_count;
static uint8_t s_last_output_low_pin;
static uint8_t s_last_output_high_pin;
static uint8_t s_reset_reason;
static uint8_t s_rwip_sleep;
static uint8_t s_sleep_mode;
static uint8_t s_deep_sleep_wake_pin;
static unsigned int s_cpu_sleep_count;
static unsigned int s_cpu_wakeup_count;
static unsigned int s_deep_sleep_count;
static unsigned int s_reboot_count;
static uint32_t s_reboot_reason;

rwip_time_t rwip_time_get(void) { return s_now; }

void h2_bk3633_power_sdk_fake_set_time(uint32_t half_slots,
                                       uint16_t half_microseconds) {
    s_now.hs = half_slots;
    s_now.hus = half_microseconds;
}

void h2_bk3633_power_sdk_fake_reset(void) {
    memset(s_gpio, 1, sizeof(s_gpio));
    s_now = (rwip_time_t){0};
    s_adc_raw = 0u;
    s_adc_result = H2_PAL_ERR_WOULD_BLOCK;
    s_adc_start_count = 0u;
    s_adc_abort_count = 0u;
    s_adc_init_count = 0u;
    s_adc_init_channel = 0xffu;
    s_adc_init_mode = 0xffu;
    s_adc_deinit_count = 0u;
    s_last_output_low_pin = 0xffu;
    s_last_output_high_pin = 0xffu;
    s_reset_reason = 5u;
    s_rwip_sleep = 1u;
    s_sleep_mode = 0u;
    s_deep_sleep_wake_pin = 0xffu;
    s_cpu_sleep_count = 0u;
    s_cpu_wakeup_count = 0u;
    s_deep_sleep_count = 0u;
    s_reboot_count = 0u;
    s_reboot_reason = UINT32_MAX;
}

void h2_bk3633_power_sdk_fake_set_gpio(uint8_t pin, uint8_t level) {
    s_gpio[pin] = level != 0u ? 1u : 0u;
}

void h2_bk3633_power_sdk_fake_set_adc_ready(uint16_t raw) {
    s_adc_raw = raw;
    s_adc_result = H2_PAL_OK;
}

void h2_bk3633_power_sdk_fake_set_adc_result(h2_pal_result_t result) {
    s_adc_result = result;
}

unsigned int h2_bk3633_power_sdk_fake_adc_start_count(void) {
    return s_adc_start_count;
}

unsigned int h2_bk3633_power_sdk_fake_adc_abort_count(void) {
    return s_adc_abort_count;
}

unsigned int h2_bk3633_power_sdk_fake_adc_init_count(void) {
    return s_adc_init_count;
}

uint8_t h2_bk3633_power_sdk_fake_adc_init_channel(void) {
    return s_adc_init_channel;
}

uint8_t h2_bk3633_power_sdk_fake_adc_init_mode(void) {
    return s_adc_init_mode;
}

unsigned int h2_bk3633_power_sdk_fake_adc_deinit_count(void) {
    return s_adc_deinit_count;
}

uint8_t h2_bk3633_power_sdk_fake_last_output_low_pin(void) {
    return s_last_output_low_pin;
}

uint8_t h2_bk3633_power_sdk_fake_last_output_high_pin(void) {
    return s_last_output_high_pin;
}

void h2_bk3633_power_sdk_fake_set_reset_reason(uint8_t reason) {
    s_reset_reason = reason;
}

void h2_bk3633_power_sdk_fake_set_rwip_sleep(uint8_t result) {
    s_rwip_sleep = result;
}

unsigned int h2_bk3633_power_sdk_fake_cpu_sleep_count(void) {
    return s_cpu_sleep_count;
}

unsigned int h2_bk3633_power_sdk_fake_cpu_wakeup_count(void) {
    return s_cpu_wakeup_count;
}

unsigned int h2_bk3633_power_sdk_fake_deep_sleep_count(void) {
    return s_deep_sleep_count;
}

unsigned int h2_bk3633_power_sdk_fake_reboot_count(void) {
    return s_reboot_count;
}

uint32_t h2_bk3633_power_sdk_fake_reboot_reason(void) {
    return s_reboot_reason;
}

uint8_t h2_bk3633_power_sdk_fake_deep_sleep_wake_pin(void) {
    return s_deep_sleep_wake_pin;
}

void h2_bk3633_power_sdk_fake_adc_init(uint8_t channel, uint8_t mode) {
    ++s_adc_init_count;
    s_adc_init_channel = channel;
    s_adc_init_mode = mode;
}

h2_pal_result_t h2_bk3633_power_sdk_fake_adc_start(uint8_t channel,
                                                   uint8_t mode) {
    (void)channel;
    (void)mode;
    ++s_adc_start_count;
    s_adc_result = H2_PAL_ERR_WOULD_BLOCK;
    return H2_PAL_OK;
}

h2_pal_result_t h2_bk3633_power_sdk_fake_adc_take(uint16_t *out_raw) {
    if (s_adc_result == H2_PAL_OK) {
        *out_raw = s_adc_raw;
        s_adc_result = H2_PAL_ERR_WOULD_BLOCK;
        return H2_PAL_OK;
    }
    return s_adc_result;
}

void h2_bk3633_power_sdk_fake_adc_abort(uint8_t channel) {
    (void)channel;
    ++s_adc_abort_count;
    s_adc_result = H2_PAL_ERR_WOULD_BLOCK;
}

void h2_bk3633_power_sdk_fake_adc_deinit(uint8_t channel) {
    (void)channel;
    ++s_adc_deinit_count;
}

uint8_t h2_bk3633_power_sdk_fake_gpio_read(uint8_t pin) { return s_gpio[pin]; }

void h2_bk3633_power_sdk_fake_gpio_input_pull_up(uint8_t pin) { (void)pin; }

void h2_bk3633_power_sdk_fake_gpio_output_low(uint8_t pin) {
    s_last_output_low_pin = pin;
}

void h2_bk3633_power_sdk_fake_gpio_output_high(uint8_t pin) {
    s_last_output_high_pin = pin;
}

uint8_t h2_bk3633_power_sdk_fake_reset_reason(void) { return s_reset_reason; }

uint8_t h2_bk3633_power_sdk_fake_rwip_sleep(void) { return s_rwip_sleep; }

void h2_bk3633_power_sdk_fake_set_sleep_mode(uint8_t mode) {
    s_sleep_mode = mode;
}

void h2_bk3633_power_sdk_fake_cpu_sleep(void) { ++s_cpu_sleep_count; }

void h2_bk3633_power_sdk_fake_cpu_wakeup(void) { ++s_cpu_wakeup_count; }

void h2_bk3633_power_sdk_fake_set_deep_sleep_wake(uint8_t pin) {
    s_deep_sleep_wake_pin = pin;
}

void h2_bk3633_power_sdk_fake_deep_sleep(void) { ++s_deep_sleep_count; }

void h2_bk3633_power_sdk_fake_set_reboot_reason(uint32_t reason) {
    s_reboot_reason = reason;
}

void h2_bk3633_power_sdk_fake_reboot(void) { ++s_reboot_count; }
