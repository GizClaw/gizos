#ifndef H2_IPERF_INTERNAL_H
#define H2_IPERF_INTERNAL_H

#include "h2_iperf.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* iperf3 control-channel state bytes (iperf_api.h). */
#define H2_IPERF_STATE_TEST_START 1
#define H2_IPERF_STATE_TEST_RUNNING 2
#define H2_IPERF_STATE_TEST_END 4
#define H2_IPERF_STATE_PARAM_EXCHANGE 9
#define H2_IPERF_STATE_CREATE_STREAMS 10
#define H2_IPERF_STATE_SERVER_TERMINATE 11
#define H2_IPERF_STATE_CLIENT_TERMINATE 12
#define H2_IPERF_STATE_EXCHANGE_RESULTS 13
#define H2_IPERF_STATE_DISPLAY_RESULTS 14
#define H2_IPERF_STATE_IPERF_START 15
#define H2_IPERF_STATE_IPERF_DONE 16
#define H2_IPERF_STATE_ACCESS_DENIED (-1)
#define H2_IPERF_STATE_SERVER_ERROR (-2)

/* iperf3 error codes carried by SERVER_ERROR. */
#define H2_IPERF_IE_UNIMP 13
#define H2_IPERF_IE_NOSCTP 18
#define H2_IPERF_IE_PROTOCOL 131

/* UDP connect handshake words, compared in host order by iperf3 and
 * therefore serialized little-endian here to match every supported host. */
#define H2_IPERF_UDP_CONNECT_MSG 0x36373839u
#define H2_IPERF_UDP_CONNECT_REPLY 0x39383736u
#define H2_IPERF_UDP_LEGACY_CONNECT_REPLY 987654321u
#define H2_IPERF_UDP_HEADER_32 12u
#define H2_IPERF_UDP_HEADER_64 16u

#define H2_IPERF_MAX_JSON_LEN (64u * 1024u)
#define H2_IPERF_CLIENT_VERSION "3.21"
#define H2_IPERF_CTRL_POLL_INTERVAL_MS 50u
#define H2_IPERF_RECV_SLICE_MS 100u
#define H2_IPERF_SERVER_GRACE_MS 40000u
#define H2_IPERF_SCTP_STREAMS 16u
#define H2_IPERF_SCTP_COOKIE_LIFETIME_MS 60000u

/* ---- shared helpers (h2_iperf_ctrl.c) ---------------------------------- */

uint64_t h2_iperf_now_us(const h2_iperf_config_t *config);
uint64_t h2_iperf_now_ms(const h2_iperf_config_t *config);
void h2_iperf_sleep_ms(const h2_iperf_config_t *config, uint32_t ms);
void h2_iperf_random_fill(
    const h2_iperf_config_t *config,
    uint32_t *state,
    uint8_t *out,
    size_t len);
uint32_t h2_iperf_random_seed(const h2_iperf_config_t *config);
void h2_iperf_make_cookie(
    const h2_iperf_config_t *config,
    uint32_t *state,
    char cookie[H2_IPERF_COOKIE_SIZE]);
void h2_iperf_log(
    const h2_iperf_config_t *config,
    h2_pal_log_level_t level,
    const char *format,
    ...);
bool h2_iperf_config_is_valid(const h2_iperf_config_t *config);
void h2_iperf_write_u32_be(uint8_t *out, uint32_t value);
void h2_iperf_write_u32_le(uint8_t *out, uint32_t value);
uint32_t h2_iperf_read_u32_be(const uint8_t *in);
uint32_t h2_iperf_read_u32_le(const uint8_t *in);
void h2_iperf_write_u64_be(uint8_t *out, uint64_t value);
uint64_t h2_iperf_read_u64_be(const uint8_t *in);

h2_pal_result_t h2_iperf_ctrl_send_all(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms);
h2_pal_result_t h2_iperf_ctrl_recv_all(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms);
h2_pal_result_t h2_iperf_ctrl_send_state(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    int8_t state,
    uint32_t timeout_ms);
/** `H2_PAL_ERR_TIMEOUT` when no state byte arrived within `timeout_ms`. */
h2_pal_result_t h2_iperf_ctrl_recv_state(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    int8_t *out_state,
    uint32_t timeout_ms);
h2_pal_result_t h2_iperf_ctrl_send_json(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    const char *json,
    size_t len,
    uint32_t timeout_ms);
/** Allocates `*out_json` (NUL terminated) from `config->mem` on success. */
h2_pal_result_t h2_iperf_ctrl_recv_json(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    char **out_json,
    size_t *out_len,
    uint32_t timeout_ms);
h2_pal_result_t h2_iperf_ctrl_send_server_error(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    int32_t iperf_errno,
    uint32_t timeout_ms);

/* ---- minimal JSON (h2_iperf_json.c) ------------------------------------ */

#define H2_IPERF_JSON_MAX_DEPTH 8u

typedef struct h2_iperf_json_writer {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
    uint8_t depth;
    bool need_comma[H2_IPERF_JSON_MAX_DEPTH];
    bool after_key;
} h2_iperf_json_writer_t;

void h2_iperf_json_init(h2_iperf_json_writer_t *w, char *buf, size_t cap);
void h2_iperf_json_object_begin(h2_iperf_json_writer_t *w);
void h2_iperf_json_object_end(h2_iperf_json_writer_t *w);
void h2_iperf_json_array_begin(h2_iperf_json_writer_t *w);
void h2_iperf_json_array_end(h2_iperf_json_writer_t *w);
void h2_iperf_json_key(h2_iperf_json_writer_t *w, const char *key);
void h2_iperf_json_bool(h2_iperf_json_writer_t *w, bool value);
void h2_iperf_json_i64(h2_iperf_json_writer_t *w, int64_t value);
void h2_iperf_json_u64(h2_iperf_json_writer_t *w, uint64_t value);
void h2_iperf_json_f64(h2_iperf_json_writer_t *w, double value);
void h2_iperf_json_string(h2_iperf_json_writer_t *w, const char *value);
/** True when the document is complete and fit in the buffer. */
bool h2_iperf_json_finish(const h2_iperf_json_writer_t *w);

/** Finds the first `"key":` at any depth and returns its raw value span. */
bool h2_iperf_json_find(
    const char *json,
    size_t len,
    const char *key,
    const char **out_value,
    size_t *out_len);
bool h2_iperf_json_get_f64(const char *json, size_t len, const char *key, double *out);
bool h2_iperf_json_get_i64(const char *json, size_t len, const char *key, int64_t *out);
/** True only when the key exists and its value is the literal `true`. */
bool h2_iperf_json_get_true(const char *json, size_t len, const char *key);
bool h2_iperf_json_get_string(
    const char *json,
    size_t len,
    const char *key,
    char *out,
    size_t cap);
bool h2_iperf_json_parse_f64(const char *text, size_t len, double *out);

/* ---- data stream transport (h2_iperf_stream.c) ------------------------- */

typedef struct h2_iperf_stream {
    const h2_iperf_config_t *config;
    h2_iperf_protocol_t protocol;
    uint32_t *random_state;
    h2_pal_net_socket_t sock;
    bool owns_sock;
    h2_pal_net_addr_t peer;
    bool peer_known;

    /* SCTP association state. */
    h2_pal_sctp_association_t *assoc;
    h2_pal_sctp_state_t assoc_state;
    h2_pal_result_t assoc_reason;
    uint64_t next_deadline_ms;
    uint8_t *packet_buf;
    size_t packet_cap;
    uint64_t sctp_pending_bytes;
    uint64_t sctp_rx_messages;
    uint8_t *capture;
    size_t capture_cap;
    size_t capture_len;
    bool capture_active;
    bool capture_done;
    unsigned emit_failures;

    /* UDP receive accounting (iperf_udp_recv semantics). */
    bool counters_64bit;
    bool udp_account;
    uint64_t udp_rx_seq;
    int64_t udp_errors;
    uint64_t udp_out_of_order;
    uint64_t udp_rx_packets;
    double jitter_s;
    double prev_transit_s;
    bool udp_first_packet;

    /* UDP transmit sequence. */
    uint64_t udp_tx_seq;
} h2_iperf_stream_t;

void h2_iperf_stream_init(
    h2_iperf_stream_t *stream,
    const h2_iperf_config_t *config,
    h2_iperf_protocol_t protocol,
    uint32_t *random_state);
void h2_iperf_stream_close(h2_iperf_stream_t *stream);

h2_pal_result_t h2_iperf_stream_tcp_connect(
    h2_iperf_stream_t *stream,
    const h2_pal_net_addr_t *addr,
    uint32_t timeout_ms);
void h2_iperf_stream_tcp_adopt(h2_iperf_stream_t *stream, h2_pal_net_socket_t sock);

h2_pal_result_t h2_iperf_stream_udp_open(
    h2_iperf_stream_t *stream,
    h2_pal_net_family_t family,
    uint16_t port,
    const h2_pal_net_bind_t *bind,
    uint16_t *out_port);
void h2_iperf_stream_udp_adopt(
    h2_iperf_stream_t *stream,
    h2_pal_net_socket_t sock,
    bool owns_sock);
void h2_iperf_stream_set_peer(h2_iperf_stream_t *stream, const h2_pal_net_addr_t *peer);
/** Client side of the iperf3 UDP connect handshake. */
h2_pal_result_t h2_iperf_stream_udp_connect(
    h2_iperf_stream_t *stream,
    uint8_t *scratch,
    size_t scratch_len,
    uint32_t timeout_ms);
/** Server side: waits for the connect datagram, learns the peer, replies. */
h2_pal_result_t h2_iperf_stream_udp_accept(
    h2_iperf_stream_t *stream,
    uint8_t *scratch,
    size_t scratch_len,
    uint32_t timeout_ms);

h2_pal_result_t h2_iperf_stream_sctp_create(
    h2_iperf_stream_t *stream,
    h2_pal_sctp_role_t role,
    uint16_t local_sctp_port,
    uint16_t remote_sctp_port,
    uint16_t packet_size,
    size_t max_message_size);
/** Active side: starts the association and waits for CONNECTED. */
h2_pal_result_t h2_iperf_stream_sctp_connect(
    h2_iperf_stream_t *stream,
    uint32_t timeout_ms);
/**
 * Passive side: waits for the first datagram, derives the peer SCTP port,
 * creates the association, and waits for CONNECTED.
 */
h2_pal_result_t h2_iperf_stream_sctp_accept(
    h2_iperf_stream_t *stream,
    uint16_t local_sctp_port,
    uint16_t packet_size,
    size_t max_message_size,
    uint32_t timeout_ms);
/** Receives datagrams, feeds the association, and services timers. */
h2_pal_result_t h2_iperf_stream_sctp_pump(
    h2_iperf_stream_t *stream,
    uint32_t timeout_ms);
/** Enables copying the next received message into `capture`. */
void h2_iperf_stream_sctp_capture(
    h2_iperf_stream_t *stream,
    uint8_t *capture,
    size_t cap);
h2_pal_result_t h2_iperf_stream_sctp_shutdown(
    h2_iperf_stream_t *stream,
    uint32_t timeout_ms);

/**
 * Sends one block. TCP may consume fewer bytes than requested; UDP and SCTP
 * send whole datagrams or messages. Returns bytes consumed or a PAL error.
 */
int h2_iperf_stream_send(
    h2_iperf_stream_t *stream,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms);
/**
 * Receives payload. TCP and UDP copy into `buf`; SCTP only accounts bytes
 * delivered by the association. Returns bytes, `H2_PAL_ERR_TIMEOUT` when
 * nothing arrived, or `H2_PAL_ERR_CLOSED` when the peer finished.
 */
int h2_iperf_stream_recv(
    h2_iperf_stream_t *stream,
    uint8_t *buf,
    size_t cap,
    uint32_t timeout_ms);

void h2_iperf_udp_write_header(
    uint8_t *buf,
    uint64_t now_us,
    uint64_t sequence,
    bool counters_64bit);
void h2_iperf_udp_account(
    h2_iperf_stream_t *stream,
    const uint8_t *buf,
    size_t len,
    uint64_t now_us);

/* ---- shared data phase --------------------------------------------------- */

typedef struct h2_iperf_run {
    h2_iperf_stream_t *stream;
    h2_pal_net_socket_t ctrl;
    uint8_t *block;
    size_t block_len;
    uint64_t bitrate_bps;
    uint32_t duration_ms;
    uint64_t byte_limit;
    /* Outputs. */
    uint64_t start_us;
    uint64_t end_us;
    uint64_t bytes;
    uint64_t packets;
    int8_t ctrl_state;
    bool ctrl_closed;
} h2_iperf_run_t;

/**
 * Sends blocks until the duration or byte limit is reached, a control byte
 * arrives (stored in `ctrl_state`), or the stream fails.
 */
h2_pal_result_t h2_iperf_run_sender(h2_iperf_run_t *run);
/**
 * Receives until the duration or byte limit is reached, a control byte
 * arrives, or the peer closes the stream.
 */
h2_pal_result_t h2_iperf_run_receiver(h2_iperf_run_t *run);

/** Builds the iperf3 result JSON for one stream. */
bool h2_iperf_build_results_json(
    char *buf,
    size_t cap,
    bool sender,
    const h2_iperf_stream_stats_t *stats);
/** Parses the peer result JSON into `stats`. */
bool h2_iperf_parse_results_json(
    const char *json,
    size_t len,
    h2_iperf_stream_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
