#ifndef H2_RUNTIME_SYSTEM_STATE_H
#define H2_RUNTIME_SYSTEM_STATE_H

/* Scope: Runtime system state typed read APIs. */

#include "h2_runtime_component.h"
#include "h2_runtime_types.h"
#include "h2/pal/core/h2_pal_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_runtime_system_gpio_irq_state {
    int reserved;
} h2_runtime_system_gpio_irq_state_t;

typedef struct h2_runtime_system_wifi_sta_state {
    int reserved;
} h2_runtime_system_wifi_sta_state_t;

typedef struct h2_runtime_system_wifi_ap_state {
    int reserved;
} h2_runtime_system_wifi_ap_state_t;

typedef struct h2_runtime_system_ble_state {
    int reserved;
} h2_runtime_system_ble_state_t;

typedef struct h2_runtime_system_modem_state {
    int reserved;
} h2_runtime_system_modem_state_t;

typedef struct h2_runtime_system_mqtt_state {
    int reserved;
} h2_runtime_system_mqtt_state_t;

typedef struct h2_runtime_system_webrtc_state {
    int reserved;
} h2_runtime_system_webrtc_state_t;

h2_pal_result_t h2_runtime_system_state_wifi_sta(const h2_runtime_t *runtime, h2_runtime_system_wifi_sta_state_t *out_state);
h2_pal_result_t h2_runtime_system_state_wifi_ap(const h2_runtime_t *runtime, h2_runtime_system_wifi_ap_state_t *out_state);
h2_pal_result_t h2_runtime_system_state_gpio_irq(const h2_runtime_t *runtime, h2_runtime_system_gpio_irq_state_t *out_state);
h2_pal_result_t h2_runtime_system_state_ble(const h2_runtime_t *runtime, h2_runtime_system_ble_state_t *out_state);
h2_pal_result_t h2_runtime_system_state_modem(const h2_runtime_t *runtime, h2_runtime_system_modem_state_t *out_state);
h2_pal_result_t h2_runtime_system_state_mqtt(const h2_runtime_t *runtime, h2_runtime_system_mqtt_state_t *out_state);
h2_pal_result_t h2_runtime_system_state_webrtc(const h2_runtime_t *runtime, h2_runtime_system_webrtc_state_t *out_state);

#ifdef __cplusplus
}
#endif

#endif
