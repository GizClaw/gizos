#ifndef H2_RUNTIME_INTERNAL_H
#define H2_RUNTIME_INTERNAL_H

#include "h2_runtime.h"

#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef H2_RUNTIME_SYSTEM_EVENT_SUBSCRIPTION_MAX
#define H2_RUNTIME_SYSTEM_EVENT_SUBSCRIPTION_MAX \
    ((size_t)H2_PAL_SYSTEM_EVENT_TYPE_COUNT - 1u)
#endif

#define H2_RUNTIME_RADIO_BUTTON_TRANSITION_EVENT_MAX 3u
#define H2_RUNTIME_BUTTON_PUSH_EDGE_QUEUE_CAPACITY 16u

typedef enum h2_runtime_input_source_kind {
    H2_RUNTIME_INPUT_SOURCE_NONE = 0,
    H2_RUNTIME_INPUT_SOURCE_SINGLE_BUTTON,
    H2_RUNTIME_INPUT_SOURCE_RADIO_BUTTON,
    H2_RUNTIME_INPUT_SOURCE_NFC_READER,
    H2_RUNTIME_INPUT_SOURCE_IMU,
    H2_RUNTIME_INPUT_SOURCE_BATTERY,
    H2_RUNTIME_INPUT_SOURCE_TEMPERATURE,
} h2_runtime_input_source_kind_t;

#define H2_RUNTIME_SYSTEM_EVENT_SCHEMA_MEMBERS \
    h2_runtime_system_event_gpio_irq_t gpio_irq; \
    h2_runtime_system_event_wifi_sta_t wifi_sta; \
    h2_runtime_system_event_wifi_ap_t wifi_ap; \
    h2_runtime_system_event_wifi_ap_client_t wifi_ap_client; \
    h2_runtime_system_event_ble_connection_t ble_connection; \
    h2_runtime_system_event_ble_advertising_set_t ble_advertising_set; \
    h2_runtime_system_event_ble_connection_params_t ble_connection_params; \
    h2_runtime_system_event_ble_disconnected_t ble_disconnected; \
    h2_runtime_system_event_ble_mtu_t ble_mtu; \
    h2_runtime_system_event_ble_subscription_t ble_subscription; \
    h2_runtime_system_event_ble_gatt_client_value_t ble_gatt_client_value; \
    h2_runtime_system_event_modem_error_t modem_error; \
    h2_runtime_system_event_modem_sim_t modem_sim; \
    h2_runtime_system_event_modem_registration_t modem_registration; \
    h2_runtime_system_event_modem_packet_t modem_packet; \
    h2_runtime_system_event_modem_signal_t modem_signal; \
    h2_runtime_system_event_modem_data_t modem_data; \
    h2_runtime_system_event_modem_call_t modem_call; \
    h2_runtime_system_event_netif_default_changed_t netif_default_changed

typedef union h2_runtime_queued_payload {
    unsigned char bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    H2_RUNTIME_SYSTEM_EVENT_SCHEMA_MEMBERS;
} h2_runtime_queued_payload_t;

typedef union h2_runtime_system_event_schema {
    H2_RUNTIME_SYSTEM_EVENT_SCHEMA_MEMBERS;
} h2_runtime_system_event_schema_t;

#undef H2_RUNTIME_SYSTEM_EVENT_SCHEMA_MEMBERS

#define H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(type) \
    _Static_assert(sizeof(type) <= H2_RUNTIME_EVENT_PAYLOAD_MAX, \
                   #type " exceeds H2_RUNTIME_EVENT_PAYLOAD_MAX")

H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_gpio_irq_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_wifi_sta_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_wifi_ap_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_wifi_ap_client_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_ble_connection_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_ble_advertising_set_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_ble_connection_params_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_ble_disconnected_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_ble_mtu_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_ble_subscription_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_ble_gatt_client_value_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_modem_error_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_modem_sim_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_modem_registration_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_modem_packet_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_modem_signal_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_modem_data_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_modem_call_t);
H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS(h2_runtime_system_event_netif_default_changed_t);

#undef H2_RUNTIME_ASSERT_QUEUED_PAYLOAD_FITS

_Static_assert(sizeof(h2_runtime_queued_payload_t) == H2_RUNTIME_EVENT_PAYLOAD_MAX,
               "queued payload union must match the Runtime payload upper bound");

typedef struct h2_runtime_queued_event {
    h2_runtime_event_kind_t kind;
    h2_runtime_component_t component;
    h2_runtime_component_id_t component_id;
    h2_runtime_sequence_t sequence;
    h2_runtime_timestamp_ms_t timestamp_ms;
    size_t payload_size;
    h2_runtime_queued_payload_t payload;
} h2_runtime_queued_event_t;

_Static_assert(offsetof(h2_runtime_queued_event_t, payload) %
                       _Alignof(h2_runtime_queued_payload_t) ==
                   0u,
               "queued payload offset must satisfy union alignment");

typedef struct h2_runtime_button_recognizer {
    int initialized;
    int is_pressed;
    h2_runtime_timestamp_ms_t pressed_at_ms;
    h2_runtime_timestamp_ms_t last_released_at_ms;
    uint16_t click_count;
} h2_runtime_button_recognizer_t;

typedef struct h2_runtime_button_push_edge {
    h2_pal_periph_id_t periph_id;
    h2_runtime_button_edge_t edge;
    h2_runtime_timestamp_ms_t timestamp_ms;
} h2_runtime_button_push_edge_t;

typedef struct h2_runtime_imu_recognizer {
    int shaking;
    int free_falling;
    h2_runtime_timestamp_ms_t shake_started_ms;
    h2_runtime_timestamp_ms_t free_fall_started_ms;
    h2_runtime_timestamp_ms_t last_tilt_at_ms;
    h2_runtime_timestamp_ms_t last_flip_at_ms;
} h2_runtime_imu_recognizer_t;

typedef struct h2_runtime_input_source {
    h2_runtime_input_source_kind_t kind;
    h2_runtime_component_t component;
    h2_runtime_component_id_t component_id;
    h2_pal_periph_id_t periph_id;
    h2_pal_periph_id_t group_id;
    h2_pal_button_delivery_t button_delivery;
    int skip_poll_once;
    uint32_t poll_interval_ms;
    h2_runtime_timestamp_ms_t next_due_ms;
    h2_runtime_sequence_t sequence;
    h2_runtime_timestamp_ms_t timestamp_ms;

    h2_runtime_button_state_t button_state;
    h2_runtime_nfc_state_t nfc_state;
    h2_runtime_imu_state_t imu_state;
    h2_runtime_battery_state_t battery_state;
    h2_runtime_temperature_state_t temperature_state;

    h2_runtime_button_recognizer_t button;
    h2_runtime_imu_recognizer_t imu;
} h2_runtime_input_source_t;

typedef enum h2_runtime_input_phase {
    H2_RUNTIME_INPUT_PHASE_STOPPED = 0,
    H2_RUNTIME_INPUT_PHASE_STARTING,
    H2_RUNTIME_INPUT_PHASE_TASK_RUNNING,
    H2_RUNTIME_INPUT_PHASE_STOPPING,
    H2_RUNTIME_INPUT_PHASE_FAULTED,
} h2_runtime_input_phase_t;

typedef union h2_runtime_input_event_payload {
    h2_runtime_button_down_event_t button_down;
    h2_runtime_button_up_event_t button_up;
    h2_runtime_button_action_event_t button_action;
    h2_runtime_nfc_state_t nfc_state;
    h2_runtime_imu_gesture_event_t imu_gesture;
    h2_pal_result_t error;
} h2_runtime_input_event_payload_t;

typedef struct h2_runtime_input_pending_event {
    h2_runtime_event_kind_t kind;
    h2_runtime_component_t component;
    h2_runtime_component_id_t component_id;
    h2_runtime_sequence_t sequence;
    h2_runtime_timestamp_ms_t timestamp_ms;
    size_t payload_size;
    h2_runtime_input_event_payload_t payload;
} h2_runtime_input_pending_event_t;

typedef struct h2_runtime_input_nfc_result {
    h2_pal_periph_id_t periph_id;
    h2_pal_nfc_scan_t scan;
} h2_runtime_input_nfc_result_t;

/*
 * State publication: one writer at a time (the input poller or test control,
 * serialised by the input writer mutex) copies the current component states
 * into a retired slot and atomically switches the active index; readers pin
 * the active slot with a per-slot reader count and never block the writer.
 * The mechanism is owned by h2_runtime_state.c; input is its first producer.
 */
typedef struct h2_runtime_state_entry {
    h2_runtime_component_t component;
    h2_runtime_component_id_t component_id;
    union {
        h2_runtime_button_state_t button;
        h2_runtime_nfc_state_t nfc;
        h2_runtime_imu_state_t imu;
        h2_runtime_battery_state_t battery;
        h2_runtime_temperature_state_t temperature;
    } state;
} h2_runtime_state_entry_t;

typedef struct h2_runtime_state_bank {
    h2_runtime_state_entry_t *entries;
    size_t entry_capacity;
    size_t entry_count;
    uint64_t generation;
    h2_runtime_sequence_t event_sequence_ceiling;
} h2_runtime_state_bank_t;

#define H2_RUNTIME_STATE_SLOT_COUNT 3u

typedef struct h2_runtime_state_publication {
    atomic_int ready;
    atomic_uint active_index;
    /*
     * Guards reader_count updates: ARMv5 targets have no native atomic
     * add, so the counters use load/store under this test-and-set lock
     * (the same pattern as the runtime sequence lock).
     */
    atomic_flag reader_lock;
    atomic_uint reader_count[H2_RUNTIME_STATE_SLOT_COUNT];
    h2_runtime_state_bank_t banks[H2_RUNTIME_STATE_SLOT_COUNT];
    uint64_t copy_count;
    uint64_t switch_count;
    uint64_t deferred_count;
} h2_runtime_state_publication_t;

typedef struct h2_runtime_component_mapping {
    h2_runtime_component_t component;
    h2_runtime_component_id_t component_id;
    h2_pal_periph_id_t periph_id;
} h2_runtime_component_mapping_t;

struct h2_runtime_private {
    int initialized;
    size_t allocation_size;
    h2_pal_queue_t *event_queue;
    struct h2_runtime_test_control *test_control;

    h2_pal_mem_api_t mem_proxy;
    h2_pal_firmware_info_api_t firmware_info_proxy;
    h2_pal_log_api_t log_proxy;
    h2_pal_time_api_t time_proxy;
    h2_pal_timer_api_t timer_proxy;
    h2_pal_task_api_t task_proxy;
    h2_pal_queue_api_t queue_proxy;
    h2_pal_sync_api_t sync_proxy;
    h2_pal_fs_api_t fs_proxy;
    h2_pal_disk_api_t disk_proxy;
    h2_pal_pref_api_t pref_proxy;
    h2_pal_crypto_api_t crypto_proxy;
    h2_pal_http_api_t http_proxy;
    h2_pal_net_api_t net_proxy;
    h2_pal_netif_api_t netif_proxy;
    h2_pal_mqtt_api_t mqtt_proxy;
    h2_pal_webrtc_api_t webrtc_proxy;
    h2_pal_wifi_sta_api_t wifi_sta_proxy;
    h2_pal_wifi_ap_api_t wifi_ap_proxy;
    h2_pal_wifi_csi_api_t wifi_csi_proxy;
    h2_pal_wifi_settings_api_t wifi_settings_proxy;
    h2_pal_ble_host_api_t ble_host_proxy;
    h2_pal_modem_api_t modem_proxy;
    h2_pal_power_api_t power_proxy;
    h2_pal_display_api_t display_proxy;
    h2_pal_audio_api_t audio_proxy;
    h2_pal_audio_decoder_api_t audio_decoder_proxy;
    h2_pal_periph_api_t periph_proxy;
    h2_pal_button_api_t button_proxy;
    h2_pal_touch_api_t touch_proxy;
    h2_pal_buzzer_api_t buzzer_proxy;
    h2_pal_nfc_api_t nfc_proxy;
    h2_pal_nfc_card_emulation_api_t nfc_card_emulation_proxy;
    h2_pal_imu_api_t imu_proxy;
    h2_pal_gpio_irq_api_t gpio_irq_proxy;
    h2_pal_led_api_t led_proxy;
    h2_pal_switch_api_t switch_proxy;
    h2_pal_pwm_switch_api_t pwm_switch_proxy;
    h2_pal_input_api_t input_proxy;
    h2_pal_system_event_api_t system_event_proxy;
    h2_pal_video_decoder_api_t video_decoder_proxy;

    atomic_flag sequence_lock;
    h2_runtime_sequence_t next_sequence;
    uint32_t dropped_event_count;

    atomic_int system_event_active;
    h2_pal_system_event_subscription_t *
        system_event_subscriptions[H2_RUNTIME_SYSTEM_EVENT_SUBSCRIPTION_MAX];
    size_t system_event_subscription_count;

    atomic_int input_phase;
    atomic_int input_stop_requested;
    atomic_int input_worker_result;
    uint32_t input_tick_ms;
    uint32_t input_button_poll_interval_ms;
    uint32_t input_nfc_poll_interval_ms;
    uint32_t input_imu_poll_interval_ms;
    uint32_t input_battery_poll_interval_ms;
    uint32_t input_temperature_poll_interval_ms;
    h2_pal_task_t *input_task;
    h2_pal_task_t *input_nfc_task;
    h2_pal_queue_t *input_push_edge_queue;
    h2_pal_queue_t *input_nfc_result_queue;
    h2_pal_mutex_t *input_writer_mutex;
    int input_sources_ready;
    h2_runtime_component_mapping_t *component_mappings;
    size_t component_mapping_capacity;
    size_t component_mapping_count;
    h2_runtime_input_source_t *input_sources;
    size_t input_source_capacity;
    size_t input_source_count;
    h2_runtime_input_pending_event_t *input_pending_events;
    size_t input_pending_event_capacity;
    size_t input_pending_event_count;
    size_t event_payload_capacity;
    h2_runtime_sequence_t input_event_sequence_ceiling;
    /* State publication (owned by h2_runtime_state.c). */
    h2_runtime_state_publication_t state_publication;
    int state_dirty;
    uint64_t state_generation;
    uint32_t state_publish_interval_ms;
    h2_runtime_timestamp_ms_t state_next_publish_ms;
};

static inline int h2_runtime_ready(const h2_runtime_t *runtime) {
    return runtime != NULL && runtime->private_state != NULL &&
           runtime->private_state->initialized != 0;
}

static inline h2_runtime_timestamp_ms_t h2_runtime_now_ms(const h2_pal_time_api_t *time) {
    uint64_t now = 0u;
    if (h2_pal_time_get_monotonic_ms(time, &now) != H2_PAL_OK) {
        return 0u;
    }
    return (h2_runtime_timestamp_ms_t)now;
}

h2_runtime_sequence_t h2_runtime_next_sequence(h2_runtime_t *runtime);

h2_pal_result_t h2_runtime_emit_event(
    h2_runtime_t *runtime,
    h2_runtime_event_kind_t kind,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id,
    h2_runtime_sequence_t sequence,
    h2_runtime_timestamp_ms_t timestamp_ms,
    const void *payload,
    size_t payload_size);

h2_pal_result_t h2_runtime_enqueue_event(
    h2_runtime_t *runtime,
    const h2_runtime_queued_event_t *queued);

h2_runtime_input_source_t *h2_runtime_find_input_source(
    h2_runtime_t *runtime,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id);

const h2_runtime_input_source_t *h2_runtime_find_input_source_const(
    const h2_runtime_t *runtime,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id);

h2_runtime_component_t h2_runtime_component_from_periph_type(
    h2_pal_periph_type_t type);

h2_pal_result_t h2_runtime_build_component_mappings(
    h2_runtime_t *runtime,
    const h2_runtime_component_mapper_t *mapper);

const h2_runtime_component_mapping_t *h2_runtime_find_component_mapping_by_periph(
    const h2_runtime_t *runtime,
    h2_pal_periph_id_t periph_id);

/* State publication (h2_runtime_state.c). */
typedef void (*h2_runtime_state_fill_fn)(
    h2_runtime_t *runtime,
    h2_runtime_state_bank_t *bank);
h2_pal_result_t h2_runtime_state_publication_init(h2_runtime_t *runtime);
void h2_runtime_state_publication_deinit(h2_runtime_t *runtime);
int h2_runtime_state_publication_ready(const h2_runtime_t *runtime);
void h2_runtime_state_set_publish_interval(
    h2_runtime_t *runtime,
    uint32_t interval_ms);
void h2_runtime_state_mark_dirty(h2_runtime_t *runtime);
/*
 * Copies the current states into a retired slot and switches the active
 * index. `fill` populates the bank entries (NULL publishes an empty bank).
 * Returns H2_PAL_ERR_WOULD_BLOCK when readers pin every retired slot.
 */
h2_pal_result_t h2_runtime_state_publish(
    h2_runtime_t *runtime,
    h2_runtime_state_fill_fn fill,
    h2_runtime_sequence_t event_sequence_ceiling);
/* Publishes a dirty bank once the publish interval elapsed (or on force). */
h2_pal_result_t h2_runtime_state_publish_if_due(
    h2_runtime_t *runtime,
    h2_runtime_timestamp_ms_t now_ms,
    int force,
    h2_runtime_state_fill_fn fill,
    h2_runtime_sequence_t event_sequence_ceiling);
h2_pal_result_t h2_runtime_state_read_begin(
    const h2_runtime_t *runtime,
    const h2_runtime_state_bank_t **out_bank,
    uint8_t *out_slot_index);
h2_pal_result_t h2_runtime_state_read_end(
    const h2_runtime_t *runtime,
    uint8_t slot_index);
/* h2_runtime_input_start() and h2_runtime_input_stop() are public API declared
 * in h2_runtime_input.h, which h2_runtime.h pulls in above. They only switch
 * the poller on and off; the writer mutex, the input source table and the
 * state publication are built once by h2_runtime_input_prepare() during init
 * and released by h2_runtime_input_release() during deinit. */
h2_pal_result_t h2_runtime_input_prepare(h2_runtime_t *runtime);
void h2_runtime_input_release(h2_runtime_t *runtime);
h2_pal_result_t h2_runtime_input_poll_once(h2_runtime_t *runtime);
h2_pal_result_t h2_runtime_input_poll_sensors_once(h2_runtime_t *runtime);
h2_pal_result_t h2_runtime_input_test_session_open(
    h2_runtime_t *runtime,
    struct h2_runtime_test_control *control);
h2_pal_result_t h2_runtime_input_test_writer_lock(h2_runtime_t *runtime);
h2_pal_result_t h2_runtime_input_test_writer_unlock(h2_runtime_t *runtime);
h2_pal_result_t h2_runtime_input_test_publish(
    h2_runtime_t *runtime);
h2_pal_result_t h2_runtime_input_test_session_close(
    h2_runtime_t *runtime);
h2_pal_result_t h2_runtime_start_system_events(h2_runtime_t *runtime);
void h2_runtime_stop_system_events(h2_runtime_t *runtime);
size_t h2_runtime_system_event_payload_capacity_min(void);

#ifdef __cplusplus
}
#endif

#endif
