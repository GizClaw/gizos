#ifndef H2_LVGL_BUTTON_INTERNAL_H
#define H2_LVGL_BUTTON_INTERNAL_H

#include "h2_lvgl_button.h"

h2_pal_result_t h2_lvgl_button_registry_claim(
    h2_lvgl_button_t *binding,
    h2_runtime_t *runtime,
    h2_pal_periph_id_t periph_id);

void h2_lvgl_button_registry_release(h2_lvgl_button_t *binding);

h2_pal_result_t h2_lvgl_button_validate_periph(
    const h2_pal_periph_info_t *info);

#endif
