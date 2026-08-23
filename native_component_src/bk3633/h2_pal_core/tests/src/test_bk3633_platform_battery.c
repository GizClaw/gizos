#include "h2_bk3633_platform_core.h"

#include "h2_bk3633_mem_test_support.h"
#include "h2_bk3633_power_sdk_fake.h"
#include "h2_libco.h"

#include <assert.h>
#include <stdlib.h>

#define BATTERY_ID 6u
#define BATTERY_ADC_PIN 0x32u
#define CHARGING_PIN 0x33u
#define COMPLETE_PIN 0x22u

typedef struct test_env {
    h2_libco_t *core;
    uint64_t now_ms;
    size_t allocations;
} test_env_t;

typedef struct read_call {
    const h2_pal_input_api_t *input;
    h2_pal_battery_reading_t reading;
    h2_pal_result_t result;
} read_call_t;

void h2_bk3633_platform_battery_test_flash_check_adc_use(void);

static void *test_alloc(void *user, size_t size) {
    test_env_t *env = user;
    void *memory = malloc(size);
    if (memory != NULL) {
        ++env->allocations;
    }
    return memory;
}

static void test_free(void *user, void *memory) {
    test_env_t *env = user;
    assert(env->allocations != 0u);
    --env->allocations;
    free(memory);
}

static uint64_t test_now(void *user) {
    return ((test_env_t *)user)->now_ms;
}

static void test_env_init(test_env_t *env) {
    const h2_libco_config_t config = {
        .user = env,
        .alloc = test_alloc,
        .free = test_free,
        .now_ms = test_now,
    };
    assert(h2_libco_create(&config, &env->core) == H2_LIBCO_OK);
}

static void test_env_deinit(test_env_t *env) {
    assert(h2_libco_destroy(&env->core) == H2_LIBCO_OK);
    assert(env->allocations == 0u);
}

static int read_entry(void *user) {
    read_call_t *call = user;
    call->result = h2_pal_input_read_battery(
        call->input, BATTERY_ID, &call->reading);
    return 0;
}

static h2_bk3633_platform_battery_t *create_battery(test_env_t *env) {
    const h2_bk3633_platform_battery_config_t config = {
        .periph_id = BATTERY_ID,
        .adc_channel = 2u,
        .adc_mode = 1u,
        .adc_gpio_pin = BATTERY_ADC_PIN,
        .charging_gpio_pin = CHARGING_PIN,
        .complete_gpio_pin = COMPLETE_PIN,
        .charging_active_level = 0u,
        .complete_active_level = 0u,
        .empty_raw = 161u,
        .full_raw = 195u,
        .sample_interval_ms = 2000u,
        .conversion_timeout_ms = 10u,
        .charge_stable_ms = 30u,
    };
    h2_bk3633_platform_battery_t *battery = NULL;

    assert(h2_bk3633_platform_battery_init(
               &config, h2_bk3633_platform_mem_api(),
               h2_libco_time_api(env->core), &battery) == H2_PAL_OK);
    assert(battery != NULL);
    assert(h2_bk3633_power_sdk_fake_last_output_high_pin() ==
           BATTERY_ADC_PIN);
    return battery;
}

static h2_pal_battery_reading_t take_sample(
    test_env_t *env, h2_bk3633_platform_battery_t *battery, uint16_t raw) {
    read_call_t call = {
        .input = h2_bk3633_platform_battery_api(battery),
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    h2_libco_task_t *task = NULL;
    size_t resumed = 0u;

    assert(h2_libco_task_start(env->core, NULL, read_entry, &call, &task) ==
           H2_LIBCO_OK);
    assert(h2_libco_schedule(env->core, 1u, &resumed) == H2_LIBCO_OK);
    assert(resumed == 1u);
    assert(h2_libco_task_join(env->core, task, NULL) == H2_LIBCO_ERR_BUSY);

    h2_bk3633_power_sdk_fake_set_adc_ready(raw);
    ++env->now_ms;
    assert(h2_libco_schedule(env->core, 1u, &resumed) == H2_LIBCO_OK);
    assert(resumed == 1u);
    assert(h2_libco_task_join(env->core, task, NULL) == H2_LIBCO_OK);
    assert(call.result == H2_PAL_OK);
    return call.reading;
}

static void test_synchronous_interpolation_and_discharge_clamp(void) {
    test_env_t env = {0};
    h2_bk3633_platform_battery_t *battery;
    h2_pal_battery_reading_t reading;

    h2_bk3633_power_sdk_fake_reset();
    test_env_init(&env);
    battery = create_battery(&env);
    reading = take_sample(&env, battery, 178u);
    assert(reading.id == BATTERY_ID);
    assert(reading.percent_x100 == 5000u);
    assert((reading.flags & H2_PAL_BATTERY_PRESENT) != 0u);
    assert((reading.flags & H2_PAL_BATTERY_HAS_PERCENT_X100) != 0u);

    env.now_ms = 2001u;
    reading = take_sample(&env, battery, 180u);
    assert(reading.percent_x100 == 5000u);

    env.now_ms = 4002u;
    reading = take_sample(&env, battery, 170u);
    assert(reading.percent_x100 == 2647u);
    h2_bk3633_platform_battery_deinit(battery);
    test_env_deinit(&env);
}

static void test_rejects_second_adc_owner(void) {
    test_env_t env = {0};
    h2_bk3633_platform_battery_t *battery;
    h2_bk3633_platform_battery_t *second = NULL;
    const h2_bk3633_platform_battery_config_t config = {
        .periph_id = BATTERY_ID,
        .adc_channel = 2u,
        .adc_mode = 1u,
        .adc_gpio_pin = BATTERY_ADC_PIN,
        .charging_gpio_pin = CHARGING_PIN,
        .complete_gpio_pin = COMPLETE_PIN,
        .charging_active_level = 0u,
        .complete_active_level = 0u,
        .empty_raw = 161u,
        .full_raw = 195u,
        .sample_interval_ms = 2000u,
        .conversion_timeout_ms = 10u,
        .charge_stable_ms = 30u,
    };

    h2_bk3633_power_sdk_fake_reset();
    test_env_init(&env);
    battery = create_battery(&env);
    assert(h2_bk3633_platform_battery_init(
               &config, h2_bk3633_platform_mem_api(),
               h2_libco_time_api(env.core), &second) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(second == NULL);
    h2_bk3633_platform_battery_deinit(battery);
    test_env_deinit(&env);
}

static void test_charge_stability_and_precedence(void) {
    test_env_t env = {0};
    h2_bk3633_platform_battery_t *battery;
    h2_pal_battery_reading_t reading;
    const h2_pal_input_api_t *input;

    h2_bk3633_power_sdk_fake_reset();
    test_env_init(&env);
    battery = create_battery(&env);
    reading = take_sample(&env, battery, 178u);
    input = h2_bk3633_platform_battery_api(battery);

    h2_bk3633_power_sdk_fake_set_gpio(CHARGING_PIN, 0u);
    h2_bk3633_power_sdk_fake_set_gpio(COMPLETE_PIN, 0u);
    env.now_ms = 10u;
    assert(h2_pal_input_read_battery(input, BATTERY_ID, &reading) == H2_PAL_OK);
    assert((reading.flags & H2_PAL_BATTERY_CHARGING) == 0u);
    env.now_ms = 40u;
    assert(h2_pal_input_read_battery(input, BATTERY_ID, &reading) == H2_PAL_OK);
    assert((reading.flags & H2_PAL_BATTERY_CHARGING) != 0u);
    assert((reading.flags & H2_PAL_BATTERY_FULL) != 0u);

    h2_bk3633_platform_battery_deinit(battery);
    test_env_deinit(&env);
}

static void test_initial_charge_state_is_immediate(void) {
    test_env_t env = {0};
    h2_bk3633_platform_battery_t *battery;

    h2_bk3633_power_sdk_fake_reset();
    h2_bk3633_power_sdk_fake_set_gpio(CHARGING_PIN, 0u);
    h2_bk3633_power_sdk_fake_set_gpio(COMPLETE_PIN, 1u);
    test_env_init(&env);
    battery = create_battery(&env);

    h2_pal_battery_reading_t reading = take_sample(&env, battery, 178u);
    assert((reading.flags & H2_PAL_BATTERY_CHARGING) != 0u);
    assert((reading.flags & H2_PAL_BATTERY_FULL) == 0u);

    h2_bk3633_platform_battery_deinit(battery);
    test_env_deinit(&env);
}

static void test_timeout_sleep_and_restore(void) {
    test_env_t env = {0};
    h2_bk3633_platform_battery_t *battery;
    read_call_t call;
    h2_libco_task_t *task = NULL;
    size_t resumed = 0u;

    h2_bk3633_power_sdk_fake_reset();
    test_env_init(&env);
    battery = create_battery(&env);
    call = (read_call_t){
        .input = h2_bk3633_platform_battery_api(battery),
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    assert(h2_libco_task_start(env.core, NULL, read_entry, &call, &task) ==
           H2_LIBCO_OK);
    assert(h2_libco_schedule(env.core, 1u, &resumed) == H2_LIBCO_OK);
    env.now_ms = 10u;
    assert(h2_libco_schedule(env.core, 1u, &resumed) == H2_LIBCO_OK);
    assert(h2_libco_task_join(env.core, task, NULL) == H2_LIBCO_OK);
    assert(call.result == H2_PAL_ERR_TIMEOUT);
    assert(h2_bk3633_power_sdk_fake_adc_abort_count() == 1u);

    assert(h2_bk3633_platform_battery_prepare_sleep(battery) == H2_PAL_OK);
    assert(h2_bk3633_power_sdk_fake_adc_deinit_count() == 1u);
    assert(h2_bk3633_power_sdk_fake_last_output_low_pin() == BATTERY_ADC_PIN);
    assert(h2_pal_input_read_battery(
               call.input, BATTERY_ID, &call.reading) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_bk3633_platform_battery_restore(battery) == H2_PAL_OK);
    assert(h2_bk3633_power_sdk_fake_last_output_high_pin() ==
           BATTERY_ADC_PIN);

    h2_bk3633_platform_battery_deinit(battery);
    test_env_deinit(&env);
}

static void test_flash_check_preemption_restarts_conversion(void) {
    test_env_t env = {0};
    h2_bk3633_platform_battery_t *battery;
    read_call_t call;
    h2_libco_task_t *task = NULL;
    size_t resumed = 0u;

    h2_bk3633_power_sdk_fake_reset();
    test_env_init(&env);
    battery = create_battery(&env);
    call = (read_call_t){
        .input = h2_bk3633_platform_battery_api(battery),
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    assert(h2_libco_task_start(env.core, NULL, read_entry, &call, &task) ==
           H2_LIBCO_OK);
    assert(h2_libco_schedule(env.core, 1u, &resumed) == H2_LIBCO_OK);
    assert(h2_bk3633_power_sdk_fake_adc_start_count() == 1u);

    h2_bk3633_platform_battery_test_flash_check_adc_use();
    assert(h2_bk3633_power_sdk_fake_adc_abort_count() == 1u);
    assert(h2_bk3633_power_sdk_fake_adc_init_count() == 2u);
    ++env.now_ms;
    assert(h2_libco_schedule(env.core, 1u, &resumed) == H2_LIBCO_OK);
    assert(h2_bk3633_power_sdk_fake_adc_start_count() == 2u);

    h2_bk3633_power_sdk_fake_set_adc_ready(178u);
    ++env.now_ms;
    assert(h2_libco_schedule(env.core, 1u, &resumed) == H2_LIBCO_OK);
    assert(h2_libco_task_join(env.core, task, NULL) == H2_LIBCO_OK);
    assert(call.result == H2_PAL_OK);
    assert(call.reading.percent_x100 == 5000u);

    h2_bk3633_platform_battery_deinit(battery);
    test_env_deinit(&env);
}

static void test_flash_check_restores_idle_adc_configuration(void) {
    test_env_t env = {0};
    h2_bk3633_platform_battery_t *battery;

    h2_bk3633_power_sdk_fake_reset();
    test_env_init(&env);
    battery = create_battery(&env);
    assert(h2_bk3633_power_sdk_fake_adc_init_count() == 1u);

    h2_bk3633_platform_battery_test_flash_check_adc_use();
    assert(h2_bk3633_power_sdk_fake_adc_abort_count() == 0u);
    assert(h2_bk3633_power_sdk_fake_adc_init_count() == 2u);
    assert(h2_bk3633_power_sdk_fake_adc_init_channel() == 2u);
    assert(h2_bk3633_power_sdk_fake_adc_init_mode() == 1u);

    h2_bk3633_platform_battery_deinit(battery);
    test_env_deinit(&env);
}

int main(void) {
    h2_bk3633_mem_test_support_init();
    test_synchronous_interpolation_and_discharge_clamp();
    test_rejects_second_adc_owner();
    test_charge_stability_and_precedence();
    test_initial_charge_state_is_immediate();
    test_timeout_sleep_and_restore();
    test_flash_check_preemption_restarts_conversion();
    test_flash_check_restores_idle_adc_configuration();
    return 0;
}
