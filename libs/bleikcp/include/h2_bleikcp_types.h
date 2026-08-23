#ifndef H2_BLEIKCP_TYPES_H
#define H2_BLEIKCP_TYPES_H

#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BLEIKCP_DEFAULT_CONV UINT32_C(0x42544b43)
#define H2_BLEIKCP_DEFAULT_MAX_DATAGRAM_LEN 512u
#define H2_BLEIKCP_DEFAULT_WINDOW 8u
#define H2_BLEIKCP_DEFAULT_INPUT_FRAME_CAPACITY 8u
#define H2_BLEIKCP_DEFAULT_BUFFER_SIZE 2048u
#define H2_BLEIKCP_MIN_ATT_MTU 53u

typedef struct h2_bleikcp h2_bleikcp_t;
typedef struct h2_bleikcp_server h2_bleikcp_server_t;

typedef enum h2_bleikcp_event {
    H2_BLEIKCP_EVENT_CONNECTED = 0,
    H2_BLEIKCP_EVENT_READY,
    H2_BLEIKCP_EVENT_DISCONNECTED,
    H2_BLEIKCP_EVENT_BACKPRESSURE,
    H2_BLEIKCP_EVENT_PROTOCOL_ERROR,
    H2_BLEIKCP_EVENT_FATAL_ERROR,
} h2_bleikcp_event_t;

typedef struct h2_bleikcp_stats {
    uint16_t att_mtu;
    uint16_t kcp_mtu;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t tx_frames;
    uint64_t rx_frames;
    uint64_t input_errors;
    uint64_t dropped_input;
    uint64_t output_blocked;
    uint64_t output_retries;
    uint64_t retransmits;
    uint64_t disconnects;
    uint32_t waitsnd;
    size_t input_high_water;
    size_t tx_high_water;
    size_t rx_high_water;
} h2_bleikcp_stats_t;

/*
 * stream is NULL for persistent-server CONNECTED and pre-session
 * PROTOCOL_ERROR notifications. Callbacks must return promptly and must not
 * close a non-NULL stream from its own worker task.
 */
typedef void (*h2_bleikcp_event_fn)(
    void *user,
    h2_bleikcp_t *stream,
    h2_bleikcp_event_t event,
    uint16_t conn_handle,
    int status);

typedef struct h2_bleikcp_api {
    const h2_pal_ble_host_api_t *ble;
    const h2_pal_task_api_t *task;
    const h2_pal_time_api_t *time;
    const h2_pal_sync_api_t *sync;
    const h2_pal_system_event_api_t *system_event;
    const h2_pal_mem_api_t *allocator;
} h2_bleikcp_api_t;

typedef struct h2_bleikcp_config {
    h2_pal_ble_uuid_t service_uuid;
    h2_pal_ble_uuid_t tx_char_uuid;
    h2_pal_ble_uuid_t rx_char_uuid;
    uint32_t conv;
    uint16_t max_datagram_len;
    uint16_t send_window;
    uint16_t recv_window;
    uint16_t input_frame_capacity;
    size_t tx_buffer_size;
    size_t rx_buffer_size;
    int nodelay;
    int interval_ms;
    int resend;
    /** Zero enables the standard KCP congestion window; nonzero disables it. */
    int no_congestion_control;
    uint32_t setup_timeout_ms;
    uint32_t output_retry_count;
    uint32_t output_retry_delay_ms;
    h2_pal_task_options_t worker_task_options;
    h2_pal_task_options_t server_task_options;
    h2_bleikcp_event_fn on_event;
    void *user;
} h2_bleikcp_config_t;

#ifdef __cplusplus
}
#endif

#endif
