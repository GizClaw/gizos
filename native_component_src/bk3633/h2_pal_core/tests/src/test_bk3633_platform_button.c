#include "h2_bk3633_interaction_sdk_fake.h"
#include "h2_bk3633_mem_test_support.h"
#include "h2_bk3633_platform_core.h"

#include <assert.h>

#define POWER_ID 10u
#define ACTION_ID 11u
#define POWER_PIN 0x04u
#define ACTION_PIN 0x06u

static const h2_bk3633_platform_button_config_t s_configs[] = {
    {
        .periph_id = POWER_ID,
        .gpio_pin = POWER_PIN,
        .active_level = 0u,
        .pull = H2_BK3633_PLATFORM_BUTTON_PULL_UP,
        .debounce_ms = 30u,
    },
    {
        .periph_id = ACTION_ID,
        .gpio_pin = ACTION_PIN,
        .active_level = 0u,
        .pull = H2_BK3633_PLATFORM_BUTTON_PULL_UP,
        .debounce_ms = 30u,
    },
};

static h2_bk3633_platform_button_t *create_buttons(void)
{
    h2_bk3633_platform_button_t *buttons = NULL;
    assert(h2_bk3633_platform_button_init(
               s_configs,
               sizeof(s_configs) / sizeof(s_configs[0]),
               h2_bk3633_platform_mem_api(),
               h2_bk3633_interaction_sdk_fake_time_api(),
               &buttons) == H2_PAL_OK);
    assert(buttons != NULL);
    return buttons;
}

static h2_pal_button_state_t read_button(
    h2_bk3633_platform_button_t *buttons,
    h2_pal_periph_id_t id)
{
    h2_pal_single_button_reading_t reading;
    assert(h2_pal_button_read_single_button(
               h2_bk3633_platform_button_api(buttons), id, &reading) ==
           H2_PAL_OK);
    assert(reading.id == id);
    return reading.state;
}

static void test_debounces_press_release_and_cancels_candidate(void)
{
    h2_bk3633_interaction_sdk_fake_reset();
    h2_bk3633_platform_button_t *buttons = create_buttons();

    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_interaction_sdk_fake_set_gpio(POWER_PIN, 0u);
    h2_bk3633_interaction_sdk_fake_set_time(100u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_interaction_sdk_fake_set_time(129u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_RELEASED);

    h2_bk3633_interaction_sdk_fake_set_gpio(POWER_PIN, 1u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_interaction_sdk_fake_set_gpio(POWER_PIN, 0u);
    h2_bk3633_interaction_sdk_fake_set_time(200u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_interaction_sdk_fake_set_time(230u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_PRESSED);

    h2_bk3633_interaction_sdk_fake_set_gpio(POWER_PIN, 1u);
    h2_bk3633_interaction_sdk_fake_set_time(240u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_PRESSED);
    h2_bk3633_interaction_sdk_fake_set_time(270u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_platform_button_deinit(buttons);
}

static void test_buttons_have_independent_candidates(void)
{
    h2_bk3633_interaction_sdk_fake_reset();
    h2_bk3633_platform_button_t *buttons = create_buttons();

    h2_bk3633_interaction_sdk_fake_set_gpio(POWER_PIN, 0u);
    h2_bk3633_interaction_sdk_fake_set_time(10u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_interaction_sdk_fake_set_gpio(ACTION_PIN, 0u);
    h2_bk3633_interaction_sdk_fake_set_time(20u);
    assert(read_button(buttons, ACTION_ID) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_interaction_sdk_fake_set_time(40u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_PRESSED);
    assert(read_button(buttons, ACTION_ID) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_interaction_sdk_fake_set_time(50u);
    assert(read_button(buttons, ACTION_ID) == H2_PAL_BUTTON_STATE_PRESSED);
    h2_bk3633_platform_button_deinit(buttons);
}

static void test_failures_do_not_advance_state(void)
{
    h2_bk3633_interaction_sdk_fake_reset();
    h2_bk3633_platform_button_t *buttons = create_buttons();
    h2_pal_single_button_reading_t reading = {
        .id = 99u,
        .state = H2_PAL_BUTTON_STATE_PRESSED,
    };

    h2_bk3633_interaction_sdk_fake_set_gpio(POWER_PIN, 0u);
    h2_bk3633_interaction_sdk_fake_fail_next_gpio_read(H2_PAL_ERR_IO);
    assert(h2_pal_button_read_single_button(
               h2_bk3633_platform_button_api(buttons), POWER_ID, &reading) ==
           H2_PAL_ERR_IO);
    assert(reading.id == 0u);

    h2_bk3633_interaction_sdk_fake_fail_next_time(H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_button_read_single_button(
               h2_bk3633_platform_button_api(buttons), POWER_ID, &reading) ==
           H2_PAL_ERR_TIMEOUT);
    h2_bk3633_interaction_sdk_fake_set_time(100u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_interaction_sdk_fake_set_time(130u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_PRESSED);
    assert(h2_pal_button_read_single_button(
               h2_bk3633_platform_button_api(buttons), 999u, &reading) ==
           H2_PAL_ERR_NOT_FOUND);
    h2_bk3633_platform_button_deinit(buttons);
}

static void test_active_high_and_partial_init_cleanup(void)
{
    const h2_bk3633_platform_button_config_t active_high = {
        .periph_id = 20u,
        .gpio_pin = 0x07u,
        .active_level = 1u,
        .pull = H2_BK3633_PLATFORM_BUTTON_PULL_DOWN,
        .debounce_ms = 1u,
    };
    h2_bk3633_platform_button_t *buttons = NULL;

    h2_bk3633_interaction_sdk_fake_reset();
    h2_bk3633_interaction_sdk_fake_set_gpio(0x07u, 0u);
    assert(h2_bk3633_platform_button_init(
               &active_high, 1u, h2_bk3633_platform_mem_api(),
               h2_bk3633_interaction_sdk_fake_time_api(), &buttons) ==
           H2_PAL_OK);
    h2_bk3633_interaction_sdk_fake_set_gpio(0x07u, 1u);
    h2_bk3633_interaction_sdk_fake_set_time(1u);
    assert(read_button(buttons, 20u) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_interaction_sdk_fake_set_time(2u);
    assert(read_button(buttons, 20u) == H2_PAL_BUTTON_STATE_PRESSED);
    h2_bk3633_platform_button_deinit(buttons);

    h2_bk3633_interaction_sdk_fake_reset();
    h2_bk3633_interaction_sdk_fake_fail_gpio_config_call(
        2u, H2_PAL_ERR_IO);
    buttons = (h2_bk3633_platform_button_t *)(uintptr_t)1u;
    assert(h2_bk3633_platform_button_init(
               s_configs, 2u, h2_bk3633_platform_mem_api(),
               h2_bk3633_interaction_sdk_fake_time_api(), &buttons) ==
           H2_PAL_ERR_IO);
    assert(buttons == NULL);
    assert(h2_bk3633_interaction_sdk_fake_gpio_release_count() == 1u);
}

static void test_monotonic_regression_and_init_read_failure(void)
{
    h2_bk3633_platform_button_t *buttons;
    h2_pal_single_button_reading_t reading;

    h2_bk3633_interaction_sdk_fake_reset();
    buttons = create_buttons();
    h2_bk3633_interaction_sdk_fake_set_gpio(POWER_PIN, 0u);
    h2_bk3633_interaction_sdk_fake_set_time(100u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_RELEASED);
    h2_bk3633_interaction_sdk_fake_set_time(99u);
    assert(h2_pal_button_read_single_button(
               h2_bk3633_platform_button_api(buttons), POWER_ID, &reading) ==
           H2_PAL_ERR_INVALID_STATE);
    h2_bk3633_interaction_sdk_fake_set_time(130u);
    assert(read_button(buttons, POWER_ID) == H2_PAL_BUTTON_STATE_PRESSED);
    h2_bk3633_platform_button_deinit(buttons);

    h2_bk3633_interaction_sdk_fake_reset();
    h2_bk3633_interaction_sdk_fake_fail_next_gpio_read(H2_PAL_ERR_IO);
    buttons = (h2_bk3633_platform_button_t *)(uintptr_t)1u;
    assert(h2_bk3633_platform_button_init(
               s_configs, 2u, h2_bk3633_platform_mem_api(),
               h2_bk3633_interaction_sdk_fake_time_api(), &buttons) ==
           H2_PAL_ERR_IO);
    assert(buttons == NULL);
    assert(h2_bk3633_interaction_sdk_fake_gpio_release_count() == 1u);
}

int main(void)
{
    h2_bk3633_mem_test_support_init();
    test_debounces_press_release_and_cancels_candidate();
    test_buttons_have_independent_candidates();
    test_failures_do_not_advance_state();
    test_active_high_and_partial_init_cleanup();
    test_monotonic_regression_and_init_read_failure();
    return 0;
}
