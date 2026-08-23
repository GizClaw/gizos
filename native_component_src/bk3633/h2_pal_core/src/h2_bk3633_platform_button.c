#include "h2_bk3633_platform_core.h"

#include <stdbool.h>
#include <string.h>

#if defined(H2_BK3633_INTERACTION_SDK_FAKE)
#include "h2_bk3633_interaction_sdk_fake.h"
#else
#include "gpio.h"

static h2_pal_result_t h2_bk3633_interaction_sdk_gpio_config(
    uint8_t gpio_pin,
    h2_bk3633_platform_button_pull_t pull)
{
    Pull_Type sdk_pull;

    switch (pull) {
    case H2_BK3633_PLATFORM_BUTTON_PULL_DOWN:
        sdk_pull = PULL_LOW;
        break;
    case H2_BK3633_PLATFORM_BUTTON_PULL_UP:
        sdk_pull = PULL_HIGH;
        break;
    case H2_BK3633_PLATFORM_BUTTON_PULL_NONE:
        sdk_pull = PULL_NONE;
        break;
    default:
        return H2_PAL_ERR_INVALID_ARG;
    }
    gpio_config(gpio_pin, INPUT, sdk_pull);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_interaction_sdk_gpio_read(
    uint8_t gpio_pin,
    uint8_t *out_level)
{
    *out_level = gpio_get_input(gpio_pin) != 0u ? 1u : 0u;
    return H2_PAL_OK;
}

static void h2_bk3633_interaction_sdk_gpio_release(uint8_t gpio_pin)
{
    gpio_config(gpio_pin, FLOAT, PULL_NONE);
}
#endif

typedef struct h2_bk3633_button_entry {
    h2_bk3633_platform_button_config_t config;
    h2_pal_button_state_t confirmed_state;
    h2_pal_button_state_t candidate_state;
    uint64_t candidate_since_ms;
    bool candidate_active;
    bool gpio_configured;
} h2_bk3633_button_entry_t;

struct h2_bk3633_platform_button {
    h2_pal_button_api_t api;
    const h2_pal_mem_api_t *mem;
    const h2_pal_time_api_t *time;
    h2_bk3633_button_entry_t *entries;
    size_t entry_count;
};

static bool button_gpio_pin_valid(uint8_t gpio_pin)
{
    return (gpio_pin & 0x0fu) < 8u && (gpio_pin >> 4u) < 4u;
}

static h2_pal_result_t button_configs_validate(
    const h2_bk3633_platform_button_config_t *configs,
    size_t config_count,
    const h2_pal_mem_api_t *mem,
    const h2_pal_time_api_t *time,
    h2_bk3633_platform_button_t **out_button)
{
    if (configs == NULL || config_count == 0u || mem == NULL ||
        time == NULL || out_button == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t index = 0u; index < config_count; ++index) {
        const h2_bk3633_platform_button_config_t *config = &configs[index];
        if (config->periph_id == 0u ||
            !button_gpio_pin_valid(config->gpio_pin) ||
            config->active_level > 1u || config->debounce_ms == 0u ||
            config->pull > H2_BK3633_PLATFORM_BUTTON_PULL_NONE) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        for (size_t prior = 0u; prior < index; ++prior) {
            if (configs[prior].periph_id == config->periph_id ||
                configs[prior].gpio_pin == config->gpio_pin) {
                return H2_PAL_ERR_INVALID_ARG;
            }
        }
    }
    return H2_PAL_OK;
}

static h2_bk3633_button_entry_t *button_find_entry(
    h2_bk3633_platform_button_t *button,
    h2_pal_periph_id_t id)
{
    if (button == NULL) {
        return NULL;
    }
    for (size_t index = 0u; index < button->entry_count; ++index) {
        if (button->entries[index].config.periph_id == id) {
            return &button->entries[index];
        }
    }
    return NULL;
}

static h2_pal_button_state_t button_logical_state(
    const h2_bk3633_button_entry_t *entry,
    uint8_t raw_level)
{
    return raw_level == entry->config.active_level ?
        H2_PAL_BUTTON_STATE_PRESSED : H2_PAL_BUTTON_STATE_RELEASED;
}

static h2_pal_result_t button_read_single(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t *out_reading)
{
    h2_bk3633_platform_button_t *button =
        (h2_bk3633_platform_button_t *)user;
    h2_bk3633_button_entry_t *entry;
    h2_pal_button_state_t sampled_state;
    uint8_t raw_level = 0u;
    uint64_t now_ms = 0u;
    h2_pal_result_t rc;

    *out_reading = (h2_pal_single_button_reading_t){0};
    entry = button_find_entry(button, id);
    if (entry == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    rc = h2_bk3633_interaction_sdk_gpio_read(
        entry->config.gpio_pin, &raw_level);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    sampled_state = button_logical_state(entry, raw_level);
    if (sampled_state == entry->confirmed_state) {
        entry->candidate_active = false;
    } else {
        rc = h2_pal_time_get_monotonic_ms(button->time, &now_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (!entry->candidate_active ||
            entry->candidate_state != sampled_state) {
            entry->candidate_state = sampled_state;
            entry->candidate_since_ms = now_ms;
            entry->candidate_active = true;
        } else if (now_ms < entry->candidate_since_ms) {
            return H2_PAL_ERR_INVALID_STATE;
        } else if (now_ms - entry->candidate_since_ms >=
                   entry->config.debounce_ms) {
            entry->confirmed_state = sampled_state;
            entry->candidate_active = false;
        }
    }
    *out_reading = (h2_pal_single_button_reading_t){
        .id = id,
        .state = entry->confirmed_state,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t button_read_radio(
    void *user,
    h2_pal_periph_id_t id,
    h2_pal_radio_button_group_reading_t *out_reading)
{
    (void)user;
    (void)id;
    *out_reading = (h2_pal_radio_button_group_reading_t){0};
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_button_vtable_t s_button_vtable = {
    .read_single_button = button_read_single,
    .read_radio_button_group = button_read_radio,
};

h2_pal_result_t h2_bk3633_platform_button_init(
    const h2_bk3633_platform_button_config_t *configs,
    size_t config_count,
    const h2_pal_mem_api_t *mem,
    const h2_pal_time_api_t *time,
    h2_bk3633_platform_button_t **out_button)
{
    h2_bk3633_platform_button_t *button;
    h2_pal_result_t rc = button_configs_validate(
        configs, config_count, mem, time, out_button);
    if (out_button != NULL) {
        *out_button = NULL;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (config_count > SIZE_MAX / sizeof(*button->entries)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    button = h2_pal_mem_alloc(mem, sizeof(*button));
    if (button == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(button, 0, sizeof(*button));
    button->entries = h2_pal_mem_alloc(
        mem, config_count * sizeof(*button->entries));
    if (button->entries == NULL) {
        h2_pal_mem_free(mem, button);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(button->entries, 0, config_count * sizeof(*button->entries));
    button->api = (h2_pal_button_api_t){
        .user = button,
        .vtable = &s_button_vtable,
    };
    button->mem = mem;
    button->time = time;
    button->entry_count = config_count;

    for (size_t index = 0u; index < config_count; ++index) {
        h2_bk3633_button_entry_t *entry = &button->entries[index];
        uint8_t raw_level = 0u;

        entry->config = configs[index];
        rc = h2_bk3633_interaction_sdk_gpio_config(
            entry->config.gpio_pin, entry->config.pull);
        if (rc != H2_PAL_OK) {
            h2_bk3633_platform_button_deinit(button);
            return rc;
        }
        entry->gpio_configured = true;
        rc = h2_bk3633_interaction_sdk_gpio_read(
            entry->config.gpio_pin, &raw_level);
        if (rc != H2_PAL_OK) {
            h2_bk3633_platform_button_deinit(button);
            return rc;
        }
        entry->confirmed_state = button_logical_state(entry, raw_level);
    }
    *out_button = button;
    return H2_PAL_OK;
}

const h2_pal_button_api_t *h2_bk3633_platform_button_api(
    h2_bk3633_platform_button_t *button)
{
    return button != NULL ? &button->api : NULL;
}

void h2_bk3633_platform_button_deinit(
    h2_bk3633_platform_button_t *button)
{
    if (button == NULL) {
        return;
    }
    for (size_t index = 0u; index < button->entry_count; ++index) {
        if (button->entries[index].gpio_configured) {
            h2_bk3633_interaction_sdk_gpio_release(
                button->entries[index].config.gpio_pin);
        }
    }
    h2_pal_mem_free(button->mem, button->entries);
    h2_pal_mem_free(button->mem, button);
}
