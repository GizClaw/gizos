#ifndef H2_LVGL_BUTTON_H
#define H2_LVGL_BUTTON_H

#include "h2_runtime.h"

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_lvgl_button {
    h2_runtime_t *runtime;
    h2_pal_periph_id_t periph_id;
    h2_pal_result_t last_result;
    int pressed;
} h2_lvgl_button_t;

/**
 * Bind an LVGL object to an App-owned Runtime Button component.
 *
 * The component must map to a SINGLE_BUTTON periph whose delivery mode is
 * PUSH_EDGE. The caller-owned binding and Runtime must outlive the LVGL object.
 * Runtime continues to own click and long-press recognition. Only one live
 * LVGL object may produce edges for a mapped periph; the same periph can be
 * rebound after its previous object is deleted.
 */
h2_pal_result_t h2_lvgl_button_bind(
    h2_lvgl_button_t *binding,
    h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    lv_obj_t *object);

#ifdef __cplusplus
}
#endif

#endif
