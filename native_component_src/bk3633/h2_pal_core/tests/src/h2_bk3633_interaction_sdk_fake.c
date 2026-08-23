#include "h2_bk3633_interaction_sdk_fake.h"

#include <string.h>

#define FAKE_GPIO_COUNT 32u
#define FAKE_PWM_CALL_CAPACITY 64u

typedef struct h2_bk3633_interaction_fake_state {
    uint8_t gpio_level[FAKE_GPIO_COUNT];
    size_t gpio_release_count;
    size_t gpio_config_call_count;
    size_t fail_gpio_config_call;
    h2_pal_result_t fail_gpio_config_result;
    h2_pal_result_t next_gpio_read_result;
    uint64_t now_ms;
    h2_pal_result_t next_time_result;
    h2_bk3633_interaction_pwm_call_t pwm_calls[FAKE_PWM_CALL_CAPACITY];
    size_t pwm_call_count;
    size_t fail_pwm_call;
    h2_pal_result_t fail_pwm_result;
} h2_bk3633_interaction_fake_state_t;

static h2_bk3633_interaction_fake_state_t s_fake;

static size_t fake_gpio_index(uint8_t gpio_pin)
{
    return (size_t)(gpio_pin >> 4u) * 8u + (gpio_pin & 0x0fu);
}

void h2_bk3633_interaction_sdk_fake_reset(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    memset(s_fake.gpio_level, 1, sizeof(s_fake.gpio_level));
}

void h2_bk3633_interaction_sdk_fake_set_gpio(
    uint8_t gpio_pin,
    uint8_t level)
{
    s_fake.gpio_level[fake_gpio_index(gpio_pin)] = level != 0u ? 1u : 0u;
}

void h2_bk3633_interaction_sdk_fake_set_time(uint64_t now_ms)
{
    s_fake.now_ms = now_ms;
}

void h2_bk3633_interaction_sdk_fake_fail_gpio_config_call(
    size_t one_based_call,
    h2_pal_result_t result)
{
    s_fake.fail_gpio_config_call = one_based_call;
    s_fake.fail_gpio_config_result = result;
}

void h2_bk3633_interaction_sdk_fake_fail_next_gpio_read(
    h2_pal_result_t result)
{
    s_fake.next_gpio_read_result = result;
}

void h2_bk3633_interaction_sdk_fake_fail_next_time(
    h2_pal_result_t result)
{
    s_fake.next_time_result = result;
}

void h2_bk3633_interaction_sdk_fake_fail_pwm_call(
    size_t one_based_call,
    h2_pal_result_t result)
{
    s_fake.fail_pwm_call = one_based_call;
    s_fake.fail_pwm_result = result;
}

size_t h2_bk3633_interaction_sdk_fake_gpio_release_count(void)
{
    return s_fake.gpio_release_count;
}

size_t h2_bk3633_interaction_sdk_fake_pwm_call_count(void)
{
    return s_fake.pwm_call_count;
}

const h2_bk3633_interaction_pwm_call_t *
h2_bk3633_interaction_sdk_fake_pwm_call(size_t index)
{
    return index < s_fake.pwm_call_count ? &s_fake.pwm_calls[index] : NULL;
}

h2_pal_result_t h2_bk3633_interaction_sdk_gpio_config(
    uint8_t gpio_pin,
    h2_bk3633_platform_button_pull_t pull)
{
    (void)gpio_pin;
    (void)pull;
    ++s_fake.gpio_config_call_count;
    return s_fake.gpio_config_call_count == s_fake.fail_gpio_config_call ?
        s_fake.fail_gpio_config_result : H2_PAL_OK;
}

h2_pal_result_t h2_bk3633_interaction_sdk_gpio_read(
    uint8_t gpio_pin,
    uint8_t *out_level)
{
    h2_pal_result_t result = s_fake.next_gpio_read_result;
    s_fake.next_gpio_read_result = H2_PAL_OK;
    if (result == H2_PAL_OK) {
        *out_level = s_fake.gpio_level[fake_gpio_index(gpio_pin)];
    }
    return result;
}

void h2_bk3633_interaction_sdk_gpio_release(uint8_t gpio_pin)
{
    (void)gpio_pin;
    ++s_fake.gpio_release_count;
}

static h2_pal_result_t fake_pwm_record(
    uint8_t block,
    uint8_t channel,
    bool initialize,
    bool continuous_mode,
    uint32_t end_value,
    uint32_t duty_cycle)
{
    ++s_fake.pwm_call_count;
    if (s_fake.pwm_call_count <= FAKE_PWM_CALL_CAPACITY) {
        s_fake.pwm_calls[s_fake.pwm_call_count - 1u] =
            (h2_bk3633_interaction_pwm_call_t){
                .block = block,
                .channel = channel,
                .initialize = initialize,
                .continuous_mode = continuous_mode,
                .end_value = end_value,
                .duty_cycle = duty_cycle,
            };
    }
    if (s_fake.pwm_call_count == s_fake.fail_pwm_call) {
        return s_fake.fail_pwm_result;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_bk3633_interaction_sdk_pwm_initialize(
    uint8_t block,
    uint8_t channel,
    bool continuous_mode,
    uint32_t end_value,
    uint32_t duty_cycle)
{
    return fake_pwm_record(block, channel, true, continuous_mode, end_value,
                           duty_cycle);
}

h2_pal_result_t h2_bk3633_interaction_sdk_pwm_update(
    uint8_t block,
    uint8_t channel,
    uint32_t end_value,
    uint32_t duty_cycle)
{
    return fake_pwm_record(block, channel, false, false, end_value,
                           duty_cycle);
}

static h2_pal_result_t fake_time_monotonic(
    void *user,
    uint64_t *out_ms)
{
    (void)user;
    h2_pal_result_t result = s_fake.next_time_result;
    s_fake.next_time_result = H2_PAL_OK;
    if (result == H2_PAL_OK) {
        *out_ms = s_fake.now_ms;
    }
    return result;
}

static h2_pal_result_t fake_time_wall(
    void *user,
    uint64_t *out_ms)
{
    (void)user;
    (void)out_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t fake_time_set_wall(void *user, uint64_t wall_ms)
{
    (void)user;
    (void)wall_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t fake_time_wall_status(
    void *user,
    h2_pal_time_wall_status_t *out_status)
{
    (void)user;
    (void)out_status;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t fake_time_sleep(void *user, uint32_t delay_ms)
{
    (void)user;
    (void)delay_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

const h2_pal_time_api_t *h2_bk3633_interaction_sdk_fake_time_api(void)
{
    static const h2_pal_time_vtable_t vtable = {
        .get_monotonic_ms = fake_time_monotonic,
        .get_wall_ms = fake_time_wall,
        .set_wall_ms = fake_time_set_wall,
        .get_wall_status = fake_time_wall_status,
        .sleep_ms = fake_time_sleep,
    };
    static const h2_pal_time_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
