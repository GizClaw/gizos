#ifndef H2_RUNTIME_H
#define H2_RUNTIME_H

/*
 * Scope: Umbrella include for the app-facing runtime API.
 * Include this from product app code when the full runtime surface is needed.
 */

#include "h2_pal.h"
#include "h2/pal/os/h2_pal_disk.h"
#include "h2_runtime_component.h"
#include "h2_runtime_event.h"
#include "h2_runtime_input.h"
#include "h2_runtime_input_button_defs.h"
#include "h2_runtime_input_button.h"
#include "h2_runtime_input_imu_defs.h"
#include "h2_runtime_input_imu.h"
#include "h2_runtime_input_nfc_defs.h"
#include "h2_runtime_input_nfc.h"
#include "h2_runtime_input_sensor_defs.h"
#include "h2_runtime_input_sensor.h"
#include "h2_runtime_state.h"
#include "h2_runtime_system_event.h"
#include "h2_runtime_system_state.h"
#include "h2_runtime_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY 32u
#define H2_RUNTIME_DEFAULT_INPUT_SOURCE_CAPACITY 32u
#define H2_RUNTIME_DEFAULT_COMPONENT_MAPPING_CAPACITY 32u

typedef struct h2_runtime_private h2_runtime_private_t;

typedef struct h2_runtime_config {
    const char *board;
    const char *target;
    const char *chip;

    const h2_pal_firmware_info_api_t *firmware_info;

    const h2_pal_mem_api_t *mem;

    const h2_pal_log_api_t *log;
    const h2_pal_time_api_t *time;
    const h2_pal_timer_api_t *timer;
    const h2_pal_task_api_t *task;
    const h2_pal_queue_api_t *queue;
    const h2_pal_sync_api_t *sync;

    const h2_pal_fs_api_t *fs;
    const h2_pal_disk_api_t *disk;
    const h2_pal_pref_api_t *pref;
    const h2_pal_crypto_api_t *crypto;
    const h2_pal_http_api_t *http;

    const h2_pal_net_api_t *net;
    const h2_pal_netif_api_t *netif;
    const h2_pal_mqtt_api_t *mqtt;
    const h2_pal_webrtc_api_t *webrtc;
    const h2_pal_wifi_sta_api_t *wifi_sta;
    const h2_pal_wifi_ap_api_t *wifi_ap;
    const h2_pal_wifi_csi_api_t *wifi_csi;
    const h2_pal_wifi_settings_api_t *wifi_settings;
    const h2_pal_ble_host_api_t *ble_host;
    const h2_pal_modem_api_t *modem;

    const h2_pal_power_api_t *power;
    const h2_pal_display_api_t *display;
    const h2_pal_audio_api_t *audio;
    const h2_pal_audio_decoder_api_t *audio_decoder;
    const h2_pal_periph_api_t *periph;
    const h2_pal_button_api_t *button;
    const h2_pal_touch_api_t *touch;
    const h2_pal_buzzer_api_t *buzzer;
    const h2_pal_nfc_api_t *nfc;
    const h2_pal_nfc_card_emulation_api_t *nfc_card_emulation;
    const h2_pal_imu_api_t *imu;
    const h2_pal_gpio_irq_api_t *gpio_irq;
    const h2_pal_led_api_t *led;
    const h2_pal_switch_api_t *switch_api;
    const h2_pal_pwm_switch_api_t *pwm_switch;
    const h2_pal_input_api_t *input;
    const h2_pal_system_event_api_t *system_event;
    const h2_pal_video_decoder_api_t *video_decoder;

    const h2_runtime_component_mapper_t *component_mapper;
    /**
     * Maximum mapped input sources owned by this Runtime instance.
     * Zero selects H2_RUNTIME_DEFAULT_INPUT_SOURCE_CAPACITY.
     */
    size_t input_source_capacity;
    /**
     * Maximum component mappings accepted by this Runtime instance.
     * Zero selects H2_RUNTIME_DEFAULT_COMPONENT_MAPPING_CAPACITY.
     */
    size_t component_mapping_capacity;
    /**
     * Maximum queued payload size for this Runtime instance. Zero selects
     * H2_RUNTIME_EVENT_PAYLOAD_MAX. A nonzero value must fit every compiled
     * Runtime system-event schema and must not exceed that public upper bound.
     */
    size_t event_payload_capacity;
    size_t event_queue_capacity;
    /** Target-owned policy for the Runtime state publication. */
    h2_runtime_state_config_t state;
} h2_runtime_config_t;

struct h2_runtime {
    const char *board;
    const char *target;
    const char *chip;

    const h2_pal_firmware_info_api_t *firmware_info;

    const h2_pal_mem_api_t *mem;

    const h2_pal_log_api_t *log;
    const h2_pal_time_api_t *time;
    const h2_pal_timer_api_t *timer;
    const h2_pal_task_api_t *task;
    const h2_pal_queue_api_t *queue;
    const h2_pal_sync_api_t *sync;

    const h2_pal_fs_api_t *fs;
    const h2_pal_disk_api_t *disk;
    const h2_pal_pref_api_t *pref;
    const h2_pal_crypto_api_t *crypto;
    const h2_pal_http_api_t *http;

    const h2_pal_net_api_t *net;
    const h2_pal_netif_api_t *netif;
    const h2_pal_mqtt_api_t *mqtt;
    const h2_pal_webrtc_api_t *webrtc;
    const h2_pal_wifi_sta_api_t *wifi_sta;
    const h2_pal_wifi_ap_api_t *wifi_ap;
    const h2_pal_wifi_csi_api_t *wifi_csi;
    const h2_pal_wifi_settings_api_t *wifi_settings;
    const h2_pal_ble_host_api_t *ble_host;
    const h2_pal_modem_api_t *modem;

    const h2_pal_power_api_t *power;
    const h2_pal_display_api_t *display;
    const h2_pal_audio_api_t *audio;
    const h2_pal_audio_decoder_api_t *audio_decoder;
    const h2_pal_periph_api_t *periph;
    const h2_pal_button_api_t *button;
    const h2_pal_touch_api_t *touch;
    const h2_pal_buzzer_api_t *buzzer;
    const h2_pal_nfc_api_t *nfc;
    const h2_pal_nfc_card_emulation_api_t *nfc_card_emulation;
    const h2_pal_imu_api_t *imu;
    const h2_pal_gpio_irq_api_t *gpio_irq;
    const h2_pal_led_api_t *led;
    const h2_pal_switch_api_t *switch_api;
    const h2_pal_pwm_switch_api_t *pwm_switch;
    const h2_pal_input_api_t *input;
    const h2_pal_system_event_api_t *system_event;
    const h2_pal_video_decoder_api_t *video_decoder;

    h2_runtime_private_t *private_state;
};

h2_pal_result_t h2_runtime_init(const h2_runtime_config_t *config, h2_runtime_t **out_runtime);
void h2_runtime_deinit(h2_runtime_t *runtime);
h2_pal_result_t h2_runtime_periph_id(const h2_runtime_t *runtime, h2_runtime_component_id_t component_id, h2_pal_periph_id_t *out_periph_id);

#ifdef __cplusplus
}
#endif

#endif
