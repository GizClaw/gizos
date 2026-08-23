#ifndef H2_PEER_INTERNAL_H
#define H2_PEER_INTERNAL_H

#include "h2_peer.h"
#include "providers/h2_peer_providers.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define H2_PEER_ICE_SERVER_MAX 5u
#define H2_PEER_CHANNEL_LABEL_MAX 128u
#define H2_PEER_READY_CHANNEL_COUNT 32u
#define H2_PEER_LOCAL_STREAM_COUNT 150u
#define H2_PEER_STREAM_COUNT (H2_PEER_LOCAL_STREAM_COUNT * 2u)
#define H2_PEER_SDP_MAX 4096u
#define H2_PEER_WIRE_PACKET_MAX 1500u
#define H2_PEER_CHANNEL_FREE_PENDING UINT32_C(0x80000000)

typedef struct h2_peer_ice_server {
    char *url;
    char *username;
    char *credential;
} h2_peer_ice_server_t;

typedef struct h2_peer_stream_reset {
    uint32_t generation;
    uint8_t active;
    uint8_t outgoing_submitted;
    uint8_t outgoing_completed;
    uint8_t incoming_reset;
} h2_peer_stream_reset_t;

typedef struct h2_peer_tx_item {
    uint8_t *data;
    size_t len;
    size_t capacity;
    int is_text;
} h2_peer_tx_item_t;

enum {
    H2_PEER_INPUT_SLOT_COUNT = 1,
    H2_PEER_OUTPUT_SLOT_COUNT = 4,
};

typedef struct h2_peer_round_stats {
    uint64_t count;
    uint64_t total_us;
    uint64_t max_us;
} h2_peer_round_stats_t;

struct h2_pal_webrtc_channel {
    struct h2_pal_webrtc_peer *owner;
    struct h2_pal_webrtc_channel *next;
    h2_pal_webrtc_channel_info_t info;
    char *label;
    uint32_t generation;
    atomic_int open;
    int wire_opened;
    int remote_created;
    atomic_int terminal;
    atomic_uint callback_refs;
    uint8_t ready_slot;
    h2_peer_tx_item_t *tx_storage[H2_PEER_INPUT_SLOT_COUNT];
    atomic_uchar tx_state[H2_PEER_INPUT_SLOT_COUNT];
    h2_peer_tx_item_t *rx_storage[H2_PEER_OUTPUT_SLOT_COUNT];
    atomic_uchar rx_state[H2_PEER_OUTPUT_SLOT_COUNT];
    atomic_uint rx_count;
    size_t rx_write_cursor;
    size_t rx_read_cursor;
    h2_pal_queue_t *rx_gate;
};

struct h2_pal_webrtc_peer {
    h2_peer_t *owner;
    struct h2_pal_webrtc_peer *next;
    h2_pal_webrtc_callbacks_t callbacks;
    uint32_t receive_flags;
    h2_pal_webrtc_channel_t *channels;
    h2_peer_ice_server_t ice_servers[H2_PEER_ICE_SERVER_MAX];
    size_t ice_server_count;
    _Atomic(h2_pal_webrtc_peer_state_t) state;
    h2_peer_stream_reset_t stream_resets[H2_PEER_STREAM_COUNT];
    uint16_t next_stream_id;
    uint16_t local_stream_first;
    h2_pal_result_t stream_reset_failure;
    uint16_t rtp_sequence;
    uint32_t rtp_timestamp;
    uint32_t rtp_ssrc;
    void *ice_session;
    void *dtls_session;
    void *srtp_session;
    void *sctp_session;
    void *production_pc;
    int offer_started;
    int remote_answer_set;
    int ice_open;
    int dtls_open;
    int srtp_open;
    int sctp_open;
    int production_sctp_open;
    atomic_int closed;
    atomic_uint operation_depth;
    atomic_int close_pending;
    h2_pal_queue_t *network_commands;
    h2_pal_queue_t *network_responses;
    h2_pal_queue_t *network_events;
    h2_pal_mutex_t *network_request_mutex;
    h2_pal_task_t *network_task;
    atomic_uint network_event_count;
    atomic_size_t network_event_bytes;
    atomic_uint network_receive_count;
    atomic_uint network_receive_full;
    atomic_int network_receive_wakeup_queued;
    atomic_int network_send_wakeup_queued;
    atomic_int network_stop;
    atomic_int network_stopped;
    atomic_int network_transport_result;
    _Atomic(h2_peer_tx_item_t *) rtp_pending;
    h2_peer_tx_item_t *rtp_storage;
    h2_peer_tx_item_t *opus_rx_storage[H2_PEER_OUTPUT_SLOT_COUNT];
    atomic_uchar opus_rx_state[H2_PEER_OUTPUT_SLOT_COUNT];
    atomic_uint opus_rx_count;
    size_t opus_rx_write_cursor;
    size_t opus_rx_read_cursor;
    h2_pal_queue_t *opus_rx_gate;
    _Atomic(uint32_t) channel_ready;
    uint8_t channel_round_robin;
    uint64_t perf_window_started_us;
    h2_peer_round_stats_t perf_command;
    h2_peer_round_stats_t perf_send;
    h2_peer_round_stats_t perf_transport;
    h2_peer_round_stats_t perf_idle;
    unsigned int callback_dispatch_depth;
    int network_cleanup_pending;
};

struct h2_peer {
    h2_peer_config_t config;
    h2_peer_provider_bundle_t providers;
    h2_pal_webrtc_api_t webrtc_api;
    h2_pal_webrtc_peer_t *peers;
    atomic_uint operation_depth;
    int destroying;
    int production_backend;
};

h2_pal_result_t
h2_peer_create_with_providers(const h2_peer_config_t *config,
                              const h2_peer_provider_bundle_t *providers,
                              h2_peer_t **out_peer);

h2_pal_result_t h2_peer_receive_rtp_for_test(h2_pal_webrtc_peer_t *peer,
                                             const uint8_t *packet,
                                             size_t packet_len);

void h2_peer_webrtc_on_stream_reset(
    h2_pal_webrtc_peer_t *peer, const h2_pal_sctp_stream_reset_event_t *event);

void h2_peer_webrtc_on_sctp_closed(h2_pal_webrtc_peer_t *peer);

h2_pal_result_t h2_peer_webrtc_on_remote_channel_open(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_str_t label, uint16_t stream_id,
    int ordered, int reliable);

void h2_peer_webrtc_emit_peer_state(h2_pal_webrtc_peer_t *peer,
                                    h2_pal_webrtc_peer_state_t state);

void h2_peer_webrtc_emit_channel_state(h2_pal_webrtc_channel_t *channel,
                                       h2_pal_webrtc_channel_state_t state);

void h2_peer_webrtc_emit_local_sdp(h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_sdp_type_t type,
                                   h2_pal_webrtc_str_t sdp);

h2_pal_result_t h2_peer_webrtc_emit_channel_message(
    h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_channel_t *channel,
    const uint8_t *data, size_t len, int is_text);

void h2_peer_webrtc_emit_opus_frame(h2_pal_webrtc_peer_t *peer,
                                    const uint8_t *opus, size_t opus_len);

#endif
