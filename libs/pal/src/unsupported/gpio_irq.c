#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_gpio_irq_configure(void *p0, h2_pal_periph_id_t p1, const h2_pal_gpio_irq_config_t *p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_gpio_irq_enable(void *p0, h2_pal_periph_id_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_gpio_irq_disable(void *p0, h2_pal_periph_id_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_gpio_irq_ack(void *p0, h2_pal_periph_id_t p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_gpio_irq_vtable_t unsupported_gpio_irq_vtable = {
    .configure = unsupported_gpio_irq_configure,
    .enable = unsupported_gpio_irq_enable,
    .disable = unsupported_gpio_irq_disable,
    .ack = unsupported_gpio_irq_ack,
};
static const h2_pal_gpio_irq_api_t unsupported_gpio_irq_api = { .user = NULL, .vtable = &unsupported_gpio_irq_vtable };
const h2_pal_gpio_irq_api_t *h2_pal_unsupported_gpio_irq_api(void) { return &unsupported_gpio_irq_api; }
