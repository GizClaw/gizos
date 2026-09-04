#ifndef H2_SCTP_INTERNAL_H
#define H2_SCTP_INTERNAL_H

#include "h2_sctp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H2_SCTP_CHUNK_DATA 0u
#define H2_SCTP_CHUNK_INIT 1u
#define H2_SCTP_CHUNK_INIT_ACK 2u
#define H2_SCTP_CHUNK_SACK 3u
#define H2_SCTP_CHUNK_HEARTBEAT 4u
#define H2_SCTP_CHUNK_HEARTBEAT_ACK 5u
#define H2_SCTP_CHUNK_ABORT 6u
#define H2_SCTP_CHUNK_SHUTDOWN 7u
#define H2_SCTP_CHUNK_SHUTDOWN_ACK 8u
#define H2_SCTP_CHUNK_ERROR 9u
#define H2_SCTP_CHUNK_COOKIE_ECHO 10u
#define H2_SCTP_CHUNK_COOKIE_ACK 11u
#define H2_SCTP_CHUNK_SHUTDOWN_COMPLETE 14u
#define H2_SCTP_CHUNK_I_DATA 64u
#define H2_SCTP_CHUNK_RE_CONFIG 130u
#define H2_SCTP_CHUNK_FORWARD_TSN 192u
#define H2_SCTP_CHUNK_I_FORWARD_TSN 194u

#define H2_SCTP_DATA_FLAG_END 0x01u
#define H2_SCTP_DATA_FLAG_BEGIN 0x02u
#define H2_SCTP_DATA_FLAG_UNORDERED 0x04u
#define H2_SCTP_DATA_FLAG_IMMEDIATE 0x08u

#define H2_SCTP_PARAM_STATE_COOKIE 7u
#define H2_SCTP_PARAM_OUTGOING_RESET 13u
#define H2_SCTP_PARAM_RESET_RESPONSE 16u
#define H2_SCTP_PARAM_SUPPORTED_EXTENSIONS 0x8008u
#define H2_SCTP_PARAM_FORWARD_TSN_SUPPORTED 0xc000u

#define H2_SCTP_RTO_INITIAL_MS 1000u
#define H2_SCTP_RTO_MIN_MS 200u
#define H2_SCTP_RTO_MAX_MS 60000u
#define H2_SCTP_HEARTBEAT_INTERVAL_MS 30000u
#define H2_SCTP_DELAYED_SACK_MS 20u
#define H2_SCTP_DELAYED_SACK_PACKETS 2u
#define H2_SCTP_MAX_CONTROL_RETRIES 5u
/* RFC 6525 5.2.7 H2 keeps a deferred reset out of the ordinary error counter,
 * so it needs its own bound. With the RTO backoff below this spans minutes
 * before the association gives up on a peer that never finishes the reset. */
#define H2_SCTP_MAX_RESET_IN_PROGRESS_RETRIES 10u
#define H2_SCTP_INITIAL_CWND_PACKETS 4u
/* Largest SCTP packet an IP datagram can carry; bounds inbound parsing. */
#define H2_SCTP_MAX_INBOUND_PACKET_SIZE 65535u

typedef enum h2_sctp_control_kind {
    H2_SCTP_CONTROL_NONE = 0,
    H2_SCTP_CONTROL_INIT,
    H2_SCTP_CONTROL_COOKIE_ECHO,
    H2_SCTP_CONTROL_SHUTDOWN,
    H2_SCTP_CONTROL_RESET,
} h2_sctp_control_kind_t;

typedef struct h2_sctp_stream {
    uint16_t id;
    uint16_t next_out_ssn;
    uint16_t next_in_ssn;
    uint32_t next_out_mid_ordered;
    uint32_t next_out_mid_unordered;
    uint32_t next_in_mid_ordered;
    uint32_t next_in_mid_unordered;
    bool reset_pending;
    uint32_t reset_request_sequence;
    struct h2_sctp_tx_fragment *tx_unsent;
    struct h2_sctp_tx_fragment *tx_tail;
    struct h2_sctp_stream *next;
} h2_sctp_stream_t;

typedef struct h2_sctp_tx_fragment {
    uint64_t message_id;
    uint32_t tsn;
    uint32_t message_identifier;
    uint32_t fragment_sequence;
    uint32_t ppid;
    uint16_t stream_id;
    uint8_t flags;
    h2_pal_sctp_reliability_t reliability;
    uint32_t reliability_value;
    uint64_t submitted_ms;
    uint64_t sent_ms;
    unsigned retransmits;
    unsigned miss_reports;
    bool fast_retransmit;
    bool tsn_assigned;
    bool sent;
    bool acknowledged;
    bool abandoned;
    size_t data_len;
    uint8_t *data;
    struct h2_sctp_tx_fragment *stream_next;
    struct h2_sctp_tx_fragment *next;
} h2_sctp_tx_fragment_t;

typedef struct h2_sctp_rx_fragment {
    uint32_t tsn;
    uint32_t message_identifier;
    uint32_t fragment_sequence;
    uint32_t ppid;
    uint16_t stream_id;
    uint8_t flags;
    bool interleaved;
    bool delivered;
    size_t data_len;
    uint8_t *data;
    struct h2_sctp_rx_fragment *next;
} h2_sctp_rx_fragment_t;

struct h2_sctp {
    const h2_pal_mem_api_t *mem;
    const h2_pal_crypto_api_t *crypto;
    h2_pal_sctp_api_t api;
    struct h2_pal_sctp_association *associations;
    size_t association_count;
};

struct h2_pal_sctp_association {
    h2_sctp_t *owner;
    h2_pal_sctp_association_config_t config;
    h2_pal_sctp_state_t state;
    h2_pal_result_t terminal_reason;
    bool time_initialized;
    bool in_callback;
    bool delivery_pending;
    uint64_t last_now_ms;

    uint32_t local_verification_tag;
    uint32_t peer_verification_tag;
    uint32_t next_tsn;
    uint32_t initial_tsn;
    uint32_t peer_initial_tsn;
    uint32_t cumulative_received_tsn;
    uint32_t peer_cumulative_tsn;
    uint32_t advanced_peer_ack;
    uint32_t next_reset_sequence;
    uint32_t expected_reset_sequence;
    uint16_t negotiated_inbound_streams;
    uint16_t negotiated_outbound_streams;
    bool peer_forward_tsn;
    bool peer_interleaving;
    bool peer_stream_reset;

    uint8_t cookie[32];
    bool cookie_valid;
    uint64_t cookie_expires_ms;
    uint8_t *peer_cookie;
    size_t peer_cookie_len;
    bool shutdown_pending;
    bool peer_shutdown_pending;

    uint8_t *pending_emit;
    size_t pending_emit_len;
    uint8_t *control_packet;
    size_t control_packet_len;
    h2_sctp_control_kind_t control_kind;
    unsigned control_retries;
    bool control_reset_in_progress;
    unsigned control_reset_retries;
    uint64_t control_deadline_ms;

    uint64_t rto_ms;
    uint64_t srtt_ms;
    uint64_t rttvar_ms;
    uint64_t rtt_sample_sent_ms;
    uint32_t rtt_sample_tsn;
    bool rtt_initialized;
    bool rtt_sample_pending;
    uint64_t heartbeat_deadline_ms;
    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t fast_recovery_exit_tsn;
    bool fast_recovery_active;
    uint32_t peer_receive_window;
    size_t flight_size;
    size_t send_used;
    size_t receive_used;
    uint64_t next_message_id;
    uint16_t last_scheduled_stream;
    uint8_t sack_pending_packets;

    h2_sctp_stream_t *streams;
    h2_sctp_tx_fragment_t *tx_fragments;
    h2_sctp_tx_fragment_t *tx_fragments_tail;
    h2_sctp_rx_fragment_t *rx_fragments;
    h2_sctp_rx_fragment_t *rx_fragments_tail;
    struct h2_pal_sctp_association *next;
    uint64_t sack_deadline_ms;
};

void *h2_sctp_alloc(h2_sctp_t *provider, size_t size);
void h2_sctp_free(h2_sctp_t *provider, void *pointer);
uint64_t h2_sctp_deadline_add(uint64_t now_ms, uint64_t delta_ms);
bool h2_sctp_tsn_before(uint32_t left, uint32_t right);
bool h2_sctp_tsn_after(uint32_t left, uint32_t right);

h2_pal_result_t h2_sctp_validate_operation(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms);
void h2_sctp_notify_state(
    h2_pal_sctp_association_t *association,
    h2_pal_sctp_state_t state,
    h2_pal_result_t reason);
h2_pal_result_t h2_sctp_notify_message(
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_received_message_t *message);
void h2_sctp_notify_stream_reset(
    h2_pal_sctp_association_t *association,
    uint16_t stream_id,
    h2_pal_sctp_stream_reset_direction_t direction,
    h2_pal_result_t result);
void h2_sctp_fail(
    h2_pal_sctp_association_t *association,
    h2_pal_result_t reason);

h2_pal_result_t h2_sctp_emit_chunks(
    h2_pal_sctp_association_t *association,
    uint32_t verification_tag,
    const uint8_t *chunks,
    size_t chunks_len,
    h2_sctp_control_kind_t control_kind,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_retry_pending_emit(
    h2_pal_sctp_association_t *association);
void h2_sctp_clear_control(h2_pal_sctp_association_t *association);

#endif
