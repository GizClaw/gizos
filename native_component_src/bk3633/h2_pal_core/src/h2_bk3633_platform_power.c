#include "h2_bk3633_platform_core.h"

#if defined(H2_BK3633_POWER_SDK_FAKE)
#include "h2_bk3633_power_sdk_fake.h"
#else
#include "icu.h"
#include "rwip.h"
#endif

#include <string.h>

struct h2_bk3633_platform_power {
    h2_pal_power_api_t api;
    h2_bk3633_platform_power_config_t config;
    const h2_pal_mem_api_t *mem;
    h2_pal_power_boot_info_t boot_info;
    h2_pal_power_state_t state;
    uint8_t ready;
};

#if !defined(H2_BK3633_POWER_SDK_FAKE)
#define power_sdk_reset_reason system_reset_reson
#define power_sdk_rwip_sleep rwip_sleep
#define power_sdk_set_sleep_mode icu_set_sleep_mode
#define power_sdk_cpu_sleep cpu_reduce_voltage_sleep
#define power_sdk_cpu_wakeup cpu_wakeup
#define power_sdk_set_deep_sleep_wake deep_sleep_wakeup_set
#define power_sdk_deep_sleep deep_sleep
#define power_sdk_set_reset_reason system_set_reset_reson
#define power_sdk_reboot cpu_reset
#else
#define power_sdk_reset_reason h2_bk3633_power_sdk_fake_reset_reason
#define power_sdk_rwip_sleep h2_bk3633_power_sdk_fake_rwip_sleep
#define power_sdk_set_sleep_mode h2_bk3633_power_sdk_fake_set_sleep_mode
#define power_sdk_cpu_sleep h2_bk3633_power_sdk_fake_cpu_sleep
#define power_sdk_cpu_wakeup h2_bk3633_power_sdk_fake_cpu_wakeup
#define power_sdk_set_deep_sleep_wake                                          \
    h2_bk3633_power_sdk_fake_set_deep_sleep_wake
#define power_sdk_deep_sleep h2_bk3633_power_sdk_fake_deep_sleep
#define power_sdk_set_reset_reason                                             \
    h2_bk3633_power_sdk_fake_set_reboot_reason
#define power_sdk_reboot h2_bk3633_power_sdk_fake_reboot
#define RWIP_ACTIVE 0u
#define C_FORCE_ALL_RESET 0u
#endif

static h2_pal_power_boot_info_t
power_boot_info(uint8_t raw_reason, uint32_t boot_count, uint8_t wake_gpio) {
    h2_pal_power_boot_info_t info = {
        .source = H2_PAL_POWER_BOOT_SOURCE_UNKNOWN,
        .previous_transition = H2_PAL_POWER_PREVIOUS_TRANSITION_NONE,
        .reset_reason = H2_PAL_POWER_RESET_REASON_UNKNOWN,
        .boot_count = boot_count,
    };

    switch (raw_reason) {
    case 1u:
        info.source = H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT;
        info.reset_reason = H2_PAL_POWER_RESET_REASON_WATCHDOG;
        break;
    case 2u:
    case 4u:
        info.source = H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT;
        info.previous_transition = H2_PAL_POWER_PREVIOUS_TRANSITION_REBOOT;
        info.reset_reason = H2_PAL_POWER_RESET_REASON_SOFTWARE;
        break;
    case 3u:
        info.source = H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ;
        info.source_id = wake_gpio;
        info.previous_transition = H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP;
        info.reset_reason = H2_PAL_POWER_RESET_REASON_DEEP_SLEEP;
        break;
    case 5u:
        info.source = H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT;
        info.reset_reason = H2_PAL_POWER_RESET_REASON_POWER_ON;
        break;
    default:
        info.previous_transition = H2_PAL_POWER_PREVIOUS_TRANSITION_UNKNOWN;
        break;
    }
    return info;
}

static h2_pal_result_t
power_get_capabilities(void *user,
                       h2_pal_power_capabilities_t *out_capabilities) {
    h2_bk3633_platform_power_t *power = user;

    if (power == NULL || power->ready == 0u || out_capabilities == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_capabilities->flags = H2_PAL_POWER_CAPABILITY_REBOOT |
                              H2_PAL_POWER_CAPABILITY_SLEEP |
                              H2_PAL_POWER_CAPABILITY_DEEP_SLEEP |
                              H2_PAL_POWER_CAPABILITY_BOOT_SOURCE |
                              H2_PAL_POWER_CAPABILITY_RESET_REASON;
    return H2_PAL_OK;
}

static h2_pal_result_t power_get_boot_info(void *user,
                                           h2_pal_power_boot_info_t *out_info) {
    h2_bk3633_platform_power_t *power = user;

    if (power == NULL || power->ready == 0u || out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_info = power->boot_info;
    return H2_PAL_OK;
}

static h2_pal_result_t power_get_state(void *user,
                                       h2_pal_power_state_t *out_state) {
    h2_bk3633_platform_power_t *power = user;

    if (power == NULL || power->ready == 0u || out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_state = power->state;
    return H2_PAL_OK;
}

static h2_pal_result_t power_unsupported_list(
    void *user, h2_pal_power_boot_partition_cb_t cb, void *cb_user) {
    (void)user;
    (void)cb;
    (void)cb_user;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t
power_unsupported_partition(void *user,
                            h2_pal_power_boot_partition_t *out_partition) {
    (void)user;
    (void)out_partition;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t power_unsupported_set_partition(void *user,
                                                       uint32_t id) {
    (void)user;
    (void)id;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t power_unsupported_hold(void *user, int enabled) {
    (void)user;
    (void)enabled;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t
power_unsupported_get_hold(void *user, h2_pal_power_hold_state_t *out_state) {
    (void)user;
    (void)out_state;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t power_unsupported_transition(void *user,
                                                    uint32_t reason) {
    (void)user;
    (void)reason;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t power_sleep(void *user, uint32_t reason) {
    h2_bk3633_platform_power_t *power = user;
    uint8_t sleep_state;
    uint64_t started_ms;

    (void)reason;
    if (power == NULL || power->ready == 0u ||
        power->state != H2_PAL_POWER_STATE_RUNNING) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(
        power->config.time, &started_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (;;) {
        sleep_state = power_sdk_rwip_sleep();
        if (sleep_state != RWIP_ACTIVE) {
            break;
        }
        uint64_t now_ms;
        rc = h2_pal_time_get_monotonic_ms(power->config.time, &now_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (h2_pal_time_elapsed_ms(started_ms, now_ms) >=
            power->config.readiness_timeout_ms) {
            return H2_PAL_ERR_TIMEOUT;
        }
        rc = h2_pal_time_sleep_ms(power->config.time, 1u);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    power->state = H2_PAL_POWER_STATE_SLEEPING;
    power_sdk_set_sleep_mode(sleep_state);
    power_sdk_cpu_sleep();
    power_sdk_cpu_wakeup();
    power->state = H2_PAL_POWER_STATE_RUNNING;
    return H2_PAL_OK;
}

static h2_pal_result_t power_deep_sleep(void *user, uint32_t reason) {
    h2_bk3633_platform_power_t *power = user;
    h2_pal_result_t rc;

    (void)reason;
    if (power == NULL || power->ready == 0u ||
        power->state != H2_PAL_POWER_STATE_RUNNING) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint64_t started_ms;
    rc = h2_pal_time_get_monotonic_ms(power->config.time, &started_ms);
    if (rc != H2_PAL_OK) return rc;
    for (;;) {
        rc = power->config.validate_deep_sleep_wake(
            power->config.validate_user,
            power->config.deep_sleep_wake_gpio_pin);
        if (rc != H2_PAL_ERR_WOULD_BLOCK) break;
        uint64_t now_ms;
        rc = h2_pal_time_get_monotonic_ms(power->config.time, &now_ms);
        if (rc != H2_PAL_OK) return rc;
        if (h2_pal_time_elapsed_ms(started_ms, now_ms) >=
            power->config.readiness_timeout_ms) {
            return H2_PAL_ERR_TIMEOUT;
        }
        rc = h2_pal_time_sleep_ms(power->config.time, 1u);
        if (rc != H2_PAL_OK) return rc;
    }
    if (rc != H2_PAL_OK) return rc;
    power_sdk_set_deep_sleep_wake(power->config.deep_sleep_wake_gpio_pin);
    power->state = H2_PAL_POWER_STATE_DEEP_SLEEPING;
    power_sdk_deep_sleep();
    power->state = H2_PAL_POWER_STATE_RUNNING;
    return H2_PAL_ERR_IO;
}

static h2_pal_result_t power_reboot(void *user, uint32_t reason) {
    h2_bk3633_platform_power_t *power = user;

    (void)reason;
    if (power == NULL || power->ready == 0u ||
        power->state != H2_PAL_POWER_STATE_RUNNING) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    power->state = H2_PAL_POWER_STATE_REBOOTING;
    power_sdk_set_reset_reason(C_FORCE_ALL_RESET);
    power_sdk_reboot();
    power->state = H2_PAL_POWER_STATE_RUNNING;
    return H2_PAL_ERR_IO;
}

h2_pal_result_t
h2_bk3633_platform_power_init(const h2_bk3633_platform_power_config_t *config,
                              const h2_pal_mem_api_t *mem,
                              h2_bk3633_platform_power_t **out_power) {
    static const h2_pal_power_vtable_t vtable = {
        .get_capabilities = power_get_capabilities,
        .get_boot_info = power_get_boot_info,
        .get_state = power_get_state,
        .list_boot_partitions = power_unsupported_list,
        .get_running_boot_partition = power_unsupported_partition,
        .get_next_boot_partition = power_unsupported_partition,
        .set_next_boot_partition = power_unsupported_set_partition,
        .set_hold = power_unsupported_hold,
        .get_hold = power_unsupported_get_hold,
        .shutdown = power_unsupported_transition,
        .reboot = power_reboot,
        .sleep = power_sleep,
        .deep_sleep = power_deep_sleep,
    };
    h2_bk3633_platform_power_t *power;

    if (config == NULL || config->time == NULL ||
        config->readiness_timeout_ms == 0u || mem == NULL || out_power == NULL ||
        config->boot_count == 0u || config->validate_deep_sleep_wake == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_power = NULL;
    power = h2_pal_mem_alloc(mem, sizeof(*power));
    if (power == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(power, 0, sizeof(*power));
    power->config = *config;
    power->mem = mem;
    power->boot_info = power_boot_info(power_sdk_reset_reason(),
                                       config->boot_count,
                                       config->deep_sleep_wake_gpio_pin);
    power->state = H2_PAL_POWER_STATE_RUNNING;
    power->api.user = power;
    power->api.vtable = &vtable;
    power->ready = 1u;
    *out_power = power;
    return H2_PAL_OK;
}

const h2_pal_power_api_t *
h2_bk3633_platform_power_api(h2_bk3633_platform_power_t *power) {
    return power != NULL && power->ready != 0u ? &power->api : NULL;
}

void h2_bk3633_platform_power_deinit(h2_bk3633_platform_power_t *power) {
    const h2_pal_mem_api_t *mem;

    if (power == NULL) {
        return;
    }
    mem = power->mem;
    memset(power, 0, sizeof(*power));
    h2_pal_mem_free(mem, power);
}
