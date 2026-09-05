#ifndef H2_RUNTIME_SYSTEM_STATE_H
#define H2_RUNTIME_SYSTEM_STATE_H

/* Scope: Runtime system state typed read APIs. */

#include "h2_runtime_component.h"
#include "h2_runtime_system_event.h"
#include "h2_runtime_types.h"
#include "h2/pal/core/h2_pal_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_runtime_system_gpio_irq_state {
    int reserved;
} h2_runtime_system_gpio_irq_state_t;

/**
 * Latest station snapshot the Runtime built from the Wi-Fi system events.
 *
 * Carries the same fields as the event that produced it, so a reader gets one
 * coherent moment instead of stitching transitions together itself: a snapshot
 * reporting H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_GOT_IP always carries the
 * address that arrived with it.
 *
 * `valid` is zero until the first station event, which distinguishes "nothing
 * has happened yet" from "idle".
 */
typedef struct h2_runtime_system_wifi_sta_state {
    uint8_t valid;
    h2_runtime_system_wifi_sta_status_t status;
    char ssid[H2_RUNTIME_SYSTEM_WIFI_SSID_MAX + 1u];
    size_t ssid_len;
    uint8_t bssid[H2_RUNTIME_SYSTEM_WIFI_BSSID_LEN];
    uint8_t bssid_set;
    uint8_t channel;
    int32_t rssi;
    h2_runtime_system_wifi_ip_info_t ip;
    uint8_t ip_valid;
    int32_t disconnect_reason;
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

/**
 * @brief Copy the latest completed station snapshot.
 *
 * The Runtime holds a short lock around the copy on both sides, so a caller
 * may poll this as often as it likes without any synchronisation of its own
 * and always sees one coherent moment. A sequence counter alone would only
 * detect a torn copy after racing on the snapshot, which is a data race in
 * its own right.
 *
 * @return H2_PAL_OK, or H2_PAL_ERR_INVALID_ARG for a NULL argument or a
 * Runtime that is not ready.
 */
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
