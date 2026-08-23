#include "h2_bk3633_platform_core.h"

#include "h2_bk3633_mem_test_support.h"
#include "h2_bk3633_power_sdk_fake.h"
#include "h2_libco.h"

#include <assert.h>
#include <stdlib.h>

static h2_pal_result_t s_wake_result;

typedef struct test_env {
    h2_libco_t *core;
    uint64_t now_ms;
    size_t allocations;
} test_env_t;

typedef struct sleep_call {
    const h2_pal_power_api_t *api;
    h2_pal_result_t result;
} sleep_call_t;

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

static int sleep_entry(void *user) {
    sleep_call_t *call = user;
    call->result = h2_pal_power_sleep(call->api, 7u);
    return 0;
}

static h2_pal_result_t validate_wake(void *user, uint8_t pin) {
    unsigned int *count = user;

    assert(pin == 0x04u);
    ++*count;
    return s_wake_result;
}

static h2_bk3633_platform_power_t *create_power(test_env_t *env,
                                                uint8_t reset_reason,
                                                unsigned int *wake_count) {
    const h2_bk3633_platform_power_config_t config = {
        .boot_count = 8u,
        .deep_sleep_wake_gpio_pin = 0x04u,
        .validate_deep_sleep_wake = validate_wake,
        .validate_user = wake_count,
        .time = h2_libco_time_api(env->core),
        .readiness_timeout_ms = 20u,
    };
    h2_bk3633_platform_power_t *power = NULL;

    h2_bk3633_power_sdk_fake_set_reset_reason(reset_reason);
    assert(h2_bk3633_platform_power_init(
               &config, h2_bk3633_platform_mem_api(), &power) == H2_PAL_OK);
    return power;
}

static void test_capabilities_and_boot_mapping(void) {
    test_env_t env = {0};
    h2_bk3633_platform_power_t *power;
    const h2_pal_power_api_t *api;
    h2_pal_power_capabilities_t capabilities;
    h2_pal_power_boot_info_t info;
    unsigned int wake_count = 0u;

    h2_bk3633_power_sdk_fake_reset();
    test_env_init(&env);
    power = create_power(&env, 3u, &wake_count);
    api = h2_bk3633_platform_power_api(power);
    assert(h2_pal_power_get_capabilities(api, &capabilities) == H2_PAL_OK);
    assert((capabilities.flags & H2_PAL_POWER_CAPABILITY_REBOOT) != 0u);
    assert((capabilities.flags & H2_PAL_POWER_CAPABILITY_SLEEP) != 0u);
    assert((capabilities.flags & H2_PAL_POWER_CAPABILITY_DEEP_SLEEP) != 0u);
    assert((capabilities.flags & H2_PAL_POWER_CAPABILITY_HOLD) == 0u);
    assert((capabilities.flags & H2_PAL_POWER_CAPABILITY_SHUTDOWN) == 0u);

    assert(h2_pal_power_get_boot_info(api, &info) == H2_PAL_OK);
    assert(info.source == H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ);
    assert(info.source_id == 0x04u);
    assert(info.previous_transition ==
           H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP);
    assert(info.reset_reason == H2_PAL_POWER_RESET_REASON_DEEP_SLEEP);
    assert(info.boot_count == 8u);
    h2_bk3633_platform_power_deinit(power);

    power = create_power(&env, 2u, &wake_count);
    api = h2_bk3633_platform_power_api(power);
    assert(h2_pal_power_get_boot_info(api, &info) == H2_PAL_OK);
    assert(info.previous_transition == H2_PAL_POWER_PREVIOUS_TRANSITION_REBOOT);
    assert(info.reset_reason == H2_PAL_POWER_RESET_REASON_SOFTWARE);
    h2_bk3633_platform_power_deinit(power);
    test_env_deinit(&env);
}

static void test_sleep_wake_and_retry(void) {
    test_env_t env = {0};
    h2_bk3633_platform_power_t *power;
    const h2_pal_power_api_t *api;
    unsigned int wake_count = 0u;
    sleep_call_t call;
    h2_libco_task_t *task = NULL;
    size_t resumed = 0u;

    h2_bk3633_power_sdk_fake_reset();
    test_env_init(&env);
    power = create_power(&env, 5u, &wake_count);
    api = h2_bk3633_platform_power_api(power);
    h2_bk3633_power_sdk_fake_set_rwip_sleep(0u);
    call = (sleep_call_t){.api = api, .result = H2_PAL_ERR_INVALID_STATE};
    assert(h2_libco_task_start(env.core, NULL, sleep_entry, &call, &task) ==
           H2_LIBCO_OK);
    assert(h2_libco_schedule(env.core, 1u, &resumed) == H2_LIBCO_OK);
    assert(h2_libco_task_join(env.core, task, NULL) == H2_LIBCO_ERR_BUSY);
    assert(h2_bk3633_power_sdk_fake_cpu_sleep_count() == 0u);
    h2_bk3633_power_sdk_fake_set_rwip_sleep(2u);
    ++env.now_ms;
    assert(h2_libco_schedule(env.core, 1u, &resumed) == H2_LIBCO_OK);
    assert(h2_libco_task_join(env.core, task, NULL) == H2_LIBCO_OK);
    assert(call.result == H2_PAL_OK);
    assert(h2_bk3633_power_sdk_fake_cpu_sleep_count() == 1u);
    assert(h2_bk3633_power_sdk_fake_cpu_wakeup_count() == 1u);
    h2_bk3633_platform_power_deinit(power);
    test_env_deinit(&env);
}

static void test_deep_sleep_validation_and_returned_transitions(void) {
    test_env_t env = {0};
    h2_bk3633_platform_power_t *power;
    const h2_pal_power_api_t *api;
    unsigned int wake_count = 0u;

    h2_bk3633_power_sdk_fake_reset();
    test_env_init(&env);
    power = create_power(&env, 5u, &wake_count);
    api = h2_bk3633_platform_power_api(power);
    s_wake_result = H2_PAL_ERR_BUSY;
    assert(h2_pal_power_deep_sleep(api, 9u) == H2_PAL_ERR_BUSY);
    assert(h2_bk3633_power_sdk_fake_deep_sleep_count() == 0u);

    s_wake_result = H2_PAL_OK;
    assert(h2_pal_power_deep_sleep(api, 9u) == H2_PAL_ERR_IO);
    assert(wake_count == 2u);
    assert(h2_bk3633_power_sdk_fake_deep_sleep_wake_pin() == 0x04u);
    assert(h2_bk3633_power_sdk_fake_deep_sleep_count() == 1u);
    assert(h2_pal_power_reboot(api, 11u) == H2_PAL_ERR_IO);
    assert(h2_bk3633_power_sdk_fake_reboot_count() == 1u);
    assert(h2_bk3633_power_sdk_fake_reboot_reason() == 0u);
    h2_bk3633_platform_power_deinit(power);
    test_env_deinit(&env);
}

int main(void) {
    h2_bk3633_mem_test_support_init();
    test_capabilities_and_boot_mapping();
    test_sleep_wake_and_retry();
    test_deep_sleep_validation_and_returned_transitions();
    return 0;
}
