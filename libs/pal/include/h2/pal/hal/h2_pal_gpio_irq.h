#ifndef H2_PAL_GPIO_IRQ_H
#define H2_PAL_GPIO_IRQ_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_periph.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_gpio_irq_trigger {
    H2_PAL_GPIO_IRQ_TRIGGER_UNKNOWN = 0,
    H2_PAL_GPIO_IRQ_TRIGGER_RISING,
    H2_PAL_GPIO_IRQ_TRIGGER_FALLING,
    H2_PAL_GPIO_IRQ_TRIGGER_BOTH,
    H2_PAL_GPIO_IRQ_TRIGGER_HIGH,
    H2_PAL_GPIO_IRQ_TRIGGER_LOW,
} h2_pal_gpio_irq_trigger_t;

typedef struct h2_pal_gpio_irq_event {
    h2_pal_gpio_irq_trigger_t trigger;
} h2_pal_gpio_irq_event_t;

typedef struct h2_pal_gpio_irq_config {
    h2_pal_gpio_irq_trigger_t trigger;
} h2_pal_gpio_irq_config_t;

typedef struct h2_pal_gpio_irq_vtable {
    h2_pal_result_t (*configure)(
        void *user,
        h2_pal_periph_id_t id,
        const h2_pal_gpio_irq_config_t *config);

    h2_pal_result_t (*enable)(
        void *user,
        h2_pal_periph_id_t id);

    h2_pal_result_t (*disable)(
        void *user,
        h2_pal_periph_id_t id);

    h2_pal_result_t (*ack)(
        void *user,
        h2_pal_periph_id_t id);
} h2_pal_gpio_irq_vtable_t;

typedef struct h2_pal_gpio_irq_api {
    void *user;
    const h2_pal_gpio_irq_vtable_t *vtable;
} h2_pal_gpio_irq_api_t;

static inline int h2_pal_gpio_irq_trigger_is_valid_config(
    h2_pal_gpio_irq_trigger_t trigger) {
    return trigger > H2_PAL_GPIO_IRQ_TRIGGER_UNKNOWN &&
           trigger <= H2_PAL_GPIO_IRQ_TRIGGER_LOW;
}

static inline h2_pal_result_t h2_pal_gpio_irq_configure(
    const h2_pal_gpio_irq_api_t *api,
    h2_pal_periph_id_t id,
    const h2_pal_gpio_irq_config_t *config) {
    if (config == NULL ||
        !h2_pal_gpio_irq_trigger_is_valid_config(config->trigger)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->configure == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->configure(api->user, id, config);
}

static inline h2_pal_result_t h2_pal_gpio_irq_enable(
    const h2_pal_gpio_irq_api_t *api,
    h2_pal_periph_id_t id) {
    if (api == NULL || api->vtable == NULL || api->vtable->enable == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->enable(api->user, id);
}

static inline h2_pal_result_t h2_pal_gpio_irq_disable(
    const h2_pal_gpio_irq_api_t *api,
    h2_pal_periph_id_t id) {
    if (api == NULL || api->vtable == NULL || api->vtable->disable == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->disable(api->user, id);
}

static inline h2_pal_result_t h2_pal_gpio_irq_ack(
    const h2_pal_gpio_irq_api_t *api,
    h2_pal_periph_id_t id) {
    if (api == NULL || api->vtable == NULL || api->vtable->ack == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->ack(api->user, id);
}

#ifdef __cplusplus
}
#endif

#endif
