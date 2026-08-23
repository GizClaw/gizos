#ifndef H2_PAL_PERIPH_H
#define H2_PAL_PERIPH_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_PAL_PERIPH_NAME_MAX_LEN 32u

typedef uint32_t h2_pal_periph_id_t;

typedef enum h2_pal_periph_type {
    H2_PAL_PERIPH_TYPE_ANY = 0,
    H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
    H2_PAL_PERIPH_TYPE_RADIO_BUTTON_GROUP,
    H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
    H2_PAL_PERIPH_TYPE_GPIO_IRQ,
    H2_PAL_PERIPH_TYPE_SWITCH,
    H2_PAL_PERIPH_TYPE_PWM_SWITCH,
    H2_PAL_PERIPH_TYPE_NFC_READER,
    H2_PAL_PERIPH_TYPE_IMU,
    H2_PAL_PERIPH_TYPE_BATTERY,
    H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR,
    H2_PAL_PERIPH_TYPE_LED_STRIP,
    H2_PAL_PERIPH_TYPE_BUZZER,
} h2_pal_periph_type_t;

typedef struct h2_pal_periph_radio_button_payload {
    h2_pal_periph_id_t group_id;
} h2_pal_periph_radio_button_payload_t;

/** How Runtime receives raw state changes from one single-button periph. */
typedef enum h2_pal_button_delivery {
    /** Runtime samples the current state through Button PAL. */
    H2_PAL_BUTTON_DELIVERY_POLL_STATE = 0,
    /** The producer pushes ordered DOWN/UP edges into Runtime. */
    H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
} h2_pal_button_delivery_t;

typedef struct h2_pal_periph_single_button_payload {
    h2_pal_button_delivery_t delivery;
} h2_pal_periph_single_button_payload_t;

typedef struct h2_pal_periph_gpio_irq_payload {
    uint32_t gpio_pin_id;
} h2_pal_periph_gpio_irq_payload_t;

typedef struct h2_pal_periph_switch_payload {
    uint32_t gpio_pin_id;
} h2_pal_periph_switch_payload_t;

typedef struct h2_pal_periph_pwm_switch_payload {
    uint32_t pwm_channel_id;
    uint32_t gpio_pin_id;
    uint32_t frequency_hz;
    uint16_t duty_resolution_bits;
    uint16_t reserved;
} h2_pal_periph_pwm_switch_payload_t;

typedef struct h2_pal_periph_led_strip_payload {
    uint32_t data_gpio_pin_id;
    uint16_t led_count;
    uint8_t channels_per_led;
    uint8_t reserved;
} h2_pal_periph_led_strip_payload_t;

typedef struct h2_pal_periph_buzzer_payload {
    uint32_t min_frequency_hz;
    uint32_t max_frequency_hz;
    uint8_t supports_volume;
    uint8_t reserved[3];
} h2_pal_periph_buzzer_payload_t;

typedef struct h2_pal_periph_info {
    h2_pal_periph_id_t id;
    h2_pal_periph_type_t type;
    char name[H2_PAL_PERIPH_NAME_MAX_LEN];
    const void *payload;
    size_t payload_size;
} h2_pal_periph_info_t;

typedef h2_pal_result_t (*h2_pal_periph_cb_t)(
    void *user,
    const h2_pal_periph_info_t *info);

typedef struct h2_pal_periph_vtable {
    h2_pal_result_t (*list)(
        void *user,
        h2_pal_periph_type_t type_filter,
        h2_pal_periph_cb_t cb,
        void *cb_user);

    h2_pal_result_t (*get)(
        void *user,
        h2_pal_periph_id_t id,
        h2_pal_periph_info_t *out_info);
} h2_pal_periph_vtable_t;

typedef struct h2_pal_periph_api {
    void *user;
    const h2_pal_periph_vtable_t *vtable;
} h2_pal_periph_api_t;

static inline uint32_t h2_pal_periph_source_id(h2_pal_periph_id_t id) {
    return (uint32_t)id;
}

static inline int h2_pal_periph_type_is_valid_filter(h2_pal_periph_type_t type) {
    return (unsigned int)type <= (unsigned int)H2_PAL_PERIPH_TYPE_BUZZER;
}

static inline h2_pal_result_t h2_pal_periph_list(
    const h2_pal_periph_api_t *api,
    h2_pal_periph_type_t type_filter,
    h2_pal_periph_cb_t cb,
    void *cb_user) {
    if (cb == NULL || !h2_pal_periph_type_is_valid_filter(type_filter)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->list == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->list(api->user, type_filter, cb, cb_user);
}

static inline h2_pal_result_t h2_pal_periph_get(
    const h2_pal_periph_api_t *api,
    h2_pal_periph_id_t id,
    h2_pal_periph_info_t *out_info) {
    if (out_info == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->get == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->get(api->user, id, out_info);
}

#ifdef __cplusplus
}
#endif

#endif
