#ifndef H2_RUNTIME_COMPONENT_H
#define H2_RUNTIME_COMPONENT_H

/* Scope: Runtime component kind and app-facing component ids. */

#include "h2/pal/hal/h2_pal_periph.h"
#include "h2_runtime_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_runtime_component {
    H2_RUNTIME_COMPONENT_NONE = 0,
    H2_RUNTIME_COMPONENT_SYSTEM_WIFI,
    H2_RUNTIME_COMPONENT_SYSTEM_GPIO_IRQ,
    H2_RUNTIME_COMPONENT_SYSTEM_BLE,
    H2_RUNTIME_COMPONENT_SYSTEM_MODEM,
    H2_RUNTIME_COMPONENT_BUTTON,
    H2_RUNTIME_COMPONENT_NFC_READER,
    H2_RUNTIME_COMPONENT_IMU,
    H2_RUNTIME_COMPONENT_BATTERY,
    H2_RUNTIME_COMPONENT_PWM_SWITCH,
    H2_RUNTIME_COMPONENT_LED_STRIP,
    H2_RUNTIME_COMPONENT_TEMPERATURE_SENSOR,
    H2_RUNTIME_COMPONENT_SYSTEM_NETIF,
    H2_RUNTIME_COMPONENT_BUZZER,
    /* Owner of H2_RUNTIME_EVENT_CUSTOM events: the app or library itself. */
    H2_RUNTIME_COMPONENT_APP,
} h2_runtime_component_t;

typedef h2_runtime_id_t h2_runtime_component_id_t;

#define H2_RUNTIME_COMPONENT_ID_NONE 0u

typedef struct h2_runtime_component_mapping_entry {
    h2_runtime_component_id_t component_id;
    h2_pal_periph_id_t periph_id;
} h2_runtime_component_mapping_entry_t;

/** Public component identity. PAL objects and physical peripheral ids remain
 * private to Runtime. */
typedef struct h2_runtime_component_info {
    h2_runtime_component_id_t component_id;
    h2_runtime_component_t kind;
} h2_runtime_component_info_t;

typedef h2_pal_result_t (*h2_runtime_component_mapping_cb_t)(
    void *user,
    const h2_runtime_component_mapping_entry_t *entry);

typedef struct h2_runtime_component_mapper_vtable {
    h2_pal_result_t (*list)(
        void *user,
        h2_runtime_component_t component_filter,
        h2_runtime_component_mapping_cb_t cb,
        void *cb_user);
    h2_pal_result_t (*get_periph_id)(
        void *user,
        h2_runtime_component_id_t component_id,
        h2_pal_periph_id_t *out_periph_id);
} h2_runtime_component_mapper_vtable_t;

typedef struct h2_runtime_component_mapper {
    void *user;
    const h2_runtime_component_mapper_vtable_t *vtable;
} h2_runtime_component_mapper_t;

/** Looks up one launcher-registered component by its stable Runtime id. */
h2_pal_result_t h2_runtime_component_get(
    const h2_runtime_t *runtime,
    h2_runtime_component_id_t component_id,
    h2_runtime_component_info_t *out_info);

#ifdef __cplusplus
}
#endif

#endif
