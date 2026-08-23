#include "h2_bk3633_platform_core.h"

#if defined(H2_BK3633_POWER_SDK_FAKE)
#include "h2_bk3633_power_sdk_fake.h"
#else
#include "adc.h"
#include "gpio.h"
#endif

#include <string.h>

struct h2_bk3633_platform_battery {
    h2_pal_input_api_t api;
    h2_bk3633_platform_battery_config_t config;
    const h2_pal_mem_api_t *mem;
    const h2_pal_time_api_t *time;
    h2_pal_battery_reading_t last_reading;
    uint64_t conversion_started_ms;
    uint64_t last_sample_ms;
    uint64_t charging_candidate_since_ms;
    uint64_t complete_candidate_since_ms;
    uint16_t discharge_ceiling_x100;
    uint8_t conversion_pending;
    uint8_t conversion_interrupted;
    uint8_t have_sample;
    uint8_t have_discharge_ceiling;
    uint8_t charging_candidate;
    uint8_t charging_stable;
    uint8_t complete_candidate;
    uint8_t complete_stable;
    uint8_t charge_candidates_initialized;
    uint8_t ready;
};

static h2_bk3633_platform_battery_t *s_adc_owner;

#if !defined(H2_BK3633_POWER_SDK_FAKE)
static uint8_t s_battery_adc_wait_key;

static void battery_sdk_adc_init(uint8_t channel, uint8_t mode);
static void battery_sdk_adc_abort(uint8_t channel);
static void battery_restore_adc(h2_bk3633_platform_battery_t *battery);

extern uint8_t adc_flag;
void __real_adc_isr(void);

void __wrap_adc_isr(void) {
    __real_adc_isr();
    (void)h2_bk3633_platform_libco_record_completion(
        (uintptr_t)&s_battery_adc_wait_key);
}

void __real_check_low_volt_sleep(void);

void __wrap_check_low_volt_sleep(void) {
    h2_bk3633_platform_battery_t *battery = s_adc_owner;
    bool ready = battery != NULL && battery->ready != 0u;
    bool interrupted = ready && battery->conversion_pending != 0u;

    if (interrupted) {
        battery_sdk_adc_abort(battery->config.adc_channel);
        battery->conversion_interrupted = 1u;
    }
    __real_check_low_volt_sleep();
    if (ready) {
        battery_restore_adc(battery);
    }
    if (interrupted) {
        (void)h2_bk3633_platform_libco_record_completion(
            (uintptr_t)&s_battery_adc_wait_key);
    }
}

static void battery_sdk_adc_init(uint8_t channel, uint8_t mode) {
    adc_init(channel, mode);
}

static h2_pal_result_t battery_sdk_adc_start(uint8_t channel, uint8_t mode) {
    if ((SADC_REG0X0_CFG0 & 0x03u) == 0x03u) {
        return H2_PAL_ERR_BUSY;
    }
    adc_flag = 0u;
    SADC_REG0X0_CFG0 |= SET_ADC_EN |
                        ((uint32_t)mode << POS_SADC_REG0X0_CFG0_MODE) |
                        ((uint32_t)channel << POS_SADC_REG0X0_CFG0_CHNL);
    return H2_PAL_OK;
}

static h2_pal_result_t battery_sdk_adc_take(uint16_t *out_raw) {
    if (adc_flag == 0u) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    *out_raw = (uint16_t)(SADC_REG0X4_DAT >> 2u);
    SADC_REG0X0_CFG0 &= ~(SET_ADC_EN | (0x03u << POS_SADC_REG0X0_CFG0_MODE) |
                          (0x0fu << POS_SADC_REG0X0_CFG0_CHNL));
    adc_flag = 0u;
    return H2_PAL_OK;
}

static void battery_sdk_adc_abort(uint8_t channel) {
    SADC_REG0X0_CFG0 &= ~(SET_ADC_EN | (0x03u << POS_SADC_REG0X0_CFG0_MODE) |
                          (0x0fu << POS_SADC_REG0X0_CFG0_CHNL));
    adc_flag = 0u;
    (void)channel;
}

static void battery_sdk_adc_deinit(uint8_t channel) { adc_deinit(channel); }

static uint8_t battery_sdk_gpio_read(uint8_t pin) {
    return gpio_get_input(pin) != 0u ? 1u : 0u;
}

static void battery_sdk_gpio_input_pull_up(uint8_t pin) {
    gpio_config(pin, INPUT, PULL_HIGH);
}

static void battery_sdk_gpio_output_low(uint8_t pin) {
    gpio_config(pin, OUTPUT, PULL_NONE);
    gpio_set(pin, 0u);
}

static void battery_sdk_gpio_output_high(uint8_t pin) {
    gpio_config(pin, OUTPUT, PULL_NONE);
    gpio_set(pin, 1u);
}
#else
#define battery_sdk_adc_init h2_bk3633_power_sdk_fake_adc_init
#define battery_sdk_adc_start h2_bk3633_power_sdk_fake_adc_start
#define battery_sdk_adc_take h2_bk3633_power_sdk_fake_adc_take
#define battery_sdk_adc_abort h2_bk3633_power_sdk_fake_adc_abort
#define battery_sdk_adc_deinit h2_bk3633_power_sdk_fake_adc_deinit
#define battery_sdk_gpio_read h2_bk3633_power_sdk_fake_gpio_read
#define battery_sdk_gpio_input_pull_up                                         \
    h2_bk3633_power_sdk_fake_gpio_input_pull_up
#define battery_sdk_gpio_output_low h2_bk3633_power_sdk_fake_gpio_output_low
#define battery_sdk_gpio_output_high h2_bk3633_power_sdk_fake_gpio_output_high
#endif

static void battery_restore_adc(h2_bk3633_platform_battery_t *battery) {
    battery_sdk_gpio_output_high(battery->config.adc_gpio_pin);
    battery_sdk_adc_init(battery->config.adc_channel,
                         battery->config.adc_mode);
}

#if defined(H2_BK3633_POWER_SDK_FAKE)
static void battery_test_flash_check_adc_use(
    h2_bk3633_platform_battery_t *battery) {
    if (battery == NULL || battery->ready == 0u) {
        return;
    }
    if (battery->conversion_pending != 0u) {
        battery_sdk_adc_abort(battery->config.adc_channel);
        battery->conversion_interrupted = 1u;
    }
    battery_restore_adc(battery);
}

void h2_bk3633_platform_battery_test_flash_check_adc_use(void) {
    battery_test_flash_check_adc_use(s_adc_owner);
}
#endif

static uint16_t
battery_percent_from_raw(const h2_bk3633_platform_battery_t *battery,
                         uint16_t raw) {
    uint32_t percent_x100;

    if (raw <= battery->config.empty_raw) {
        return 0u;
    }
    if (raw >= battery->config.full_raw) {
        return 10000u;
    }
    percent_x100 =
        ((uint32_t)(raw - battery->config.empty_raw) * 10000u) /
        (uint32_t)(battery->config.full_raw - battery->config.empty_raw);
    return (uint16_t)percent_x100;
}

static void battery_update_charge_pin(uint8_t initialized,
                                      uint8_t sample,
                                      uint8_t *candidate,
                                      uint8_t *stable,
                                      uint64_t *candidate_since_ms,
                                      uint64_t now_ms,
                                      uint32_t stable_ms) {
    if (initialized == 0u) {
        *candidate = sample;
        *stable = sample;
        *candidate_since_ms = now_ms;
        return;
    }
    if (sample != *candidate) {
        *candidate = sample;
        *candidate_since_ms = now_ms;
        return;
    }
    if (h2_pal_time_elapsed_ms(*candidate_since_ms, now_ms) >= stable_ms) {
        *stable = sample;
    }
}

static void battery_update_charge_state(h2_bk3633_platform_battery_t *battery,
                                        uint64_t now_ms) {
    uint8_t charging =
        battery_sdk_gpio_read(battery->config.charging_gpio_pin) ==
        battery->config.charging_active_level;
    uint8_t complete =
        battery_sdk_gpio_read(battery->config.complete_gpio_pin) ==
        battery->config.complete_active_level;

    battery_update_charge_pin(battery->charge_candidates_initialized,
                              charging,
                              &battery->charging_candidate,
                              &battery->charging_stable,
                              &battery->charging_candidate_since_ms,
                              now_ms,
                              battery->config.charge_stable_ms);
    battery_update_charge_pin(battery->charge_candidates_initialized,
                              complete,
                              &battery->complete_candidate,
                              &battery->complete_stable,
                              &battery->complete_candidate_since_ms,
                              now_ms,
                              battery->config.charge_stable_ms);
    battery->charge_candidates_initialized = 1u;
}

static uint32_t battery_flags(const h2_bk3633_platform_battery_t *battery,
                              uint16_t percent_x100) {
    uint32_t flags = H2_PAL_BATTERY_PRESENT | H2_PAL_BATTERY_HAS_PERCENT_X100;

    if (battery->charging_stable != 0u) {
        flags |= H2_PAL_BATTERY_CHARGING;
    }
    if (battery->complete_stable != 0u) {
        flags |= H2_PAL_BATTERY_FULL;
    }
    if (percent_x100 == 0u && battery->charging_stable == 0u) {
        flags |= H2_PAL_BATTERY_LOW;
    }
    return flags;
}

static void battery_publish_sample(h2_bk3633_platform_battery_t *battery,
                                   uint16_t raw,
                                   uint64_t now_ms) {
    uint16_t percent_x100 = battery_percent_from_raw(battery, raw);

    if (battery->charging_stable != 0u) {
        battery->have_discharge_ceiling = 0u;
    } else {
        if (battery->have_discharge_ceiling != 0u &&
            percent_x100 > battery->discharge_ceiling_x100) {
            percent_x100 = battery->discharge_ceiling_x100;
        }
        battery->discharge_ceiling_x100 = percent_x100;
        battery->have_discharge_ceiling = 1u;
    }
    battery->last_reading = (h2_pal_battery_reading_t){
        .id = battery->config.periph_id,
        .flags = battery_flags(battery, percent_x100),
        .percent_x100 = percent_x100,
    };
    battery->last_sample_ms = now_ms;
    battery->have_sample = 1u;
}

static h2_pal_result_t battery_read(void *user,
                                    h2_pal_periph_id_t id,
                                    h2_pal_battery_reading_t *out_reading) {
    h2_bk3633_platform_battery_t *battery = user;
    uint64_t now_ms;
    uint16_t raw;
    h2_pal_result_t rc;

    if (battery == NULL || out_reading == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (id != battery->config.periph_id) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    if (battery->ready == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = h2_pal_time_get_monotonic_ms(battery->time, &now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    battery_update_charge_state(battery, now_ms);
    if (battery->have_sample != 0u) {
        if (battery->charging_stable != 0u) {
            battery->have_discharge_ceiling = 0u;
        }
        battery->last_reading.flags =
            battery_flags(battery, battery->last_reading.percent_x100);
    }

    if (battery->have_sample != 0u &&
        h2_pal_time_elapsed_ms(battery->last_sample_ms, now_ms) <
            battery->config.sample_interval_ms) {
        *out_reading = battery->last_reading;
        return H2_PAL_OK;
    }

    rc = battery_sdk_adc_start(battery->config.adc_channel,
                               battery->config.adc_mode);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    battery->conversion_started_ms = now_ms;
    battery->conversion_pending = 1u;
    for (;;) {
        if (battery->conversion_interrupted != 0u) {
            battery->conversion_interrupted = 0u;
            rc = battery_sdk_adc_start(battery->config.adc_channel,
                                       battery->config.adc_mode);
            if (rc != H2_PAL_OK) {
                battery->conversion_pending = 0u;
                return rc;
            }
            rc = h2_pal_time_get_monotonic_ms(battery->time, &now_ms);
            if (rc != H2_PAL_OK) {
                battery->conversion_pending = 0u;
                battery_sdk_adc_abort(battery->config.adc_channel);
                return rc;
            }
            battery->conversion_started_ms = now_ms;
        }
        rc = battery_sdk_adc_take(&raw);
        if (rc == H2_PAL_OK) {
            battery->conversion_pending = 0u;
            rc = h2_pal_time_get_monotonic_ms(battery->time, &now_ms);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            battery_publish_sample(battery, raw, now_ms);
            *out_reading = battery->last_reading;
            return H2_PAL_OK;
        }
        if (rc != H2_PAL_ERR_WOULD_BLOCK) {
            battery->conversion_pending = 0u;
            battery_sdk_adc_abort(battery->config.adc_channel);
            return rc;
        }
        rc = h2_pal_time_get_monotonic_ms(battery->time, &now_ms);
        if (rc != H2_PAL_OK ||
            h2_pal_time_elapsed_ms(
                battery->conversion_started_ms, now_ms) >=
                battery->config.conversion_timeout_ms) {
            battery->conversion_pending = 0u;
            battery_sdk_adc_abort(battery->config.adc_channel);
            return rc == H2_PAL_OK ? H2_PAL_ERR_TIMEOUT : rc;
        }
#if defined(H2_BK3633_POWER_SDK_FAKE)
        rc = h2_pal_time_sleep_ms(battery->time, 1u);
#else
        uint64_t remaining = battery->config.conversion_timeout_ms -
            h2_pal_time_elapsed_ms(battery->conversion_started_ms, now_ms);
        rc = h2_bk3633_platform_libco_wait(
            (uintptr_t)&s_battery_adc_wait_key,
            remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining);
#endif
        if (rc != H2_PAL_OK) {
            battery->conversion_pending = 0u;
            battery_sdk_adc_abort(battery->config.adc_channel);
            return rc;
        }
    }
}

static h2_pal_result_t battery_read_motion(
    void *user, h2_pal_periph_id_t id, h2_pal_motion_reading_t *out_reading) {
    (void)user;
    (void)id;
    (void)out_reading;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t
battery_read_temperature(void *user,
                         h2_pal_periph_id_t id,
                         h2_pal_temperature_reading_t *out_reading) {
    (void)user;
    (void)id;
    (void)out_reading;
    return H2_PAL_ERR_UNSUPPORTED;
}

h2_pal_result_t h2_bk3633_platform_battery_init(
    const h2_bk3633_platform_battery_config_t *config,
    const h2_pal_mem_api_t *mem,
    const h2_pal_time_api_t *time,
    h2_bk3633_platform_battery_t **out_battery) {
    static const h2_pal_input_vtable_t vtable = {
        .read_motion = battery_read_motion,
        .read_battery = battery_read,
        .read_temperature = battery_read_temperature,
    };
    h2_bk3633_platform_battery_t *battery;

    if (config == NULL || mem == NULL || time == NULL || out_battery == NULL ||
        config->full_raw <= config->empty_raw ||
        config->sample_interval_ms == 0u ||
        config->conversion_timeout_ms == 0u ||
        config->charging_active_level > 1u ||
        config->complete_active_level > 1u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_battery = NULL;
    if (s_adc_owner != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    battery = h2_pal_mem_alloc(mem, sizeof(*battery));
    if (battery == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(battery, 0, sizeof(*battery));
    battery->config = *config;
    battery->mem = mem;
    battery->time = time;
    battery->api.user = battery;
    battery->api.vtable = &vtable;
    battery_sdk_gpio_input_pull_up(config->charging_gpio_pin);
    battery_sdk_gpio_input_pull_up(config->complete_gpio_pin);
    battery_restore_adc(battery);
    battery->ready = 1u;
    s_adc_owner = battery;
    *out_battery = battery;
    return H2_PAL_OK;
}

const h2_pal_input_api_t *
h2_bk3633_platform_battery_api(h2_bk3633_platform_battery_t *battery) {
    return battery != NULL && battery->ready != 0u ? &battery->api : NULL;
}

h2_pal_result_t h2_bk3633_platform_battery_prepare_sleep(
    h2_bk3633_platform_battery_t *battery) {
    if (battery == NULL || battery->ready == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (battery->conversion_pending != 0u) {
        battery_sdk_adc_abort(battery->config.adc_channel);
        battery->conversion_pending = 0u;
    }
    battery_sdk_adc_deinit(battery->config.adc_channel);
    battery_sdk_gpio_output_low(battery->config.adc_gpio_pin);
    battery->ready = 0u;
    return H2_PAL_OK;
}

h2_pal_result_t
h2_bk3633_platform_battery_restore(h2_bk3633_platform_battery_t *battery) {
    if (battery == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (battery->ready != 0u) {
        return H2_PAL_OK;
    }
    battery_restore_adc(battery);
    battery->have_sample = 0u;
    battery->ready = 1u;
    return H2_PAL_OK;
}

void h2_bk3633_platform_battery_deinit(h2_bk3633_platform_battery_t *battery) {
    const h2_pal_mem_api_t *mem;

    if (battery == NULL) {
        return;
    }
    if (s_adc_owner == battery) {
        s_adc_owner = NULL;
    }
    mem = battery->mem;
    if (battery->conversion_pending != 0u) {
        battery_sdk_adc_abort(battery->config.adc_channel);
    }
    if (battery->ready != 0u) {
        battery_sdk_adc_deinit(battery->config.adc_channel);
    }
    memset(battery, 0, sizeof(*battery));
    h2_pal_mem_free(mem, battery);
}
