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

/** Shared logical speaker state. Volume is retained while muted. */
typedef struct h2_runtime_system_audio_state {
    uint32_t volume_percent;
    uint8_t muted;
} h2_runtime_system_audio_state_t;

/** Read the Runtime-owned speaker state into caller storage. Thread-safe;
 * BUSY means another volume operation is in progress. Unsupported Audio or
 * provider read errors propagate. Never returns a borrowed state pointer. */
h2_pal_result_t h2_runtime_system_state_audio(
    const h2_runtime_t *runtime, h2_runtime_system_audio_state_t *out_state);

/** Apply and publish volume/mute together on the calling task. Only successful
 * PAL writes change Runtime state. Percent must be 0..100; muted is 0 or 1.
 * Thread-safe (BUSY on overlap); do not call from an ISR. Finish before deinit.
 * Local percent-only Audio proxy writes clear mute and publish their value. */
h2_pal_result_t h2_runtime_audio_set_volume(
    h2_runtime_t *runtime, uint32_t percent, uint8_t muted);

/**
 * Live audio amplitude, as seen by the Runtime audio proxy.
 *
 * Each field carries the peak absolute sample of the most recent PCM frame in
 * that direction, scaled to 0..100, together with the monotonic millisecond at
 * which that frame passed through the Runtime. Only S16LE frames are measured;
 * a frame in any other sample format leaves the previous value in place.
 *
 * The Runtime publishes the raw last-frame peak and never decays it: it does
 * not know how often a consumer samples this. Decay, smoothing and hold belong
 * to the consumer, which knows its own frame rate. A consumer must treat a
 * timestamp older than its own idea of "recent" as silence rather than holding
 * the last peak forever.
 *
 * `capture_updated_ms` / `playback_updated_ms` are 0 while no frame of that
 * direction has been measured.
 */
typedef struct h2_runtime_audio_levels {
    uint8_t capture_percent;    /* 0..100, peak of the most recent mic frame */
    uint8_t playback_percent;   /* 0..100, peak of the most recent played frame */
    uint64_t capture_updated_ms;   /* monotonic ms of that frame, 0 if never */
    uint64_t playback_updated_ms;
} h2_runtime_audio_levels_t;

/**
 * Read the latest capture and playback frame levels into caller storage.
 *
 * Lock-free and safe from any task; the audio hot path only stores. A NULL
 * runtime, an unready runtime or a NULL `out_levels` returns INVALID_ARG.
 * The values are observational only: they must never gate an audio path,
 * open or close a stream, or feed anything but a display.
 */
h2_pal_result_t h2_runtime_audio_get_levels(
    const h2_runtime_t *runtime, h2_runtime_audio_levels_t *out_levels);

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
