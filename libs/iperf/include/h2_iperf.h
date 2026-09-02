#ifndef H2_IPERF_H
#define H2_IPERF_H

/**
 * @file h2_iperf.h
 * @brief iperf3-compatible throughput client and server built only on PAL.
 *
 * The wire protocol (control cookie, state bytes, JSON parameter and result
 * exchange, UDP packet header and connect handshake) follows iperf 3.x so the
 * client interoperates with the official `iperf3 -s` and the server accepts
 * the official `iperf3 -c`. Data streams run over PAL raw TCP, PAL UDP, or the
 * PAL SCTP association encapsulated in UDP (RFC 6951). The library never
 * touches POSIX; tests supply concrete PAL providers.
 */

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/net/h2_pal_net.h"
#include "h2/pal/net/h2_pal_sctp.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default iperf3 control and data port. */
#define H2_IPERF_DEFAULT_PORT 5201u
/** RFC 6951 SCTP-over-UDP encapsulation port used by the Linux kernel. */
#define H2_IPERF_DEFAULT_SCTP_UDP_PORT 9899u
/** Cookie storage including the terminating NUL. */
#define H2_IPERF_COOKIE_SIZE 37u
/** Default block size for TCP streams (iperf3 DEFAULT_TCP_BLKSIZE). */
#define H2_IPERF_DEFAULT_TCP_BLOCK_LEN (128u * 1024u)
/** Default block size for UDP streams (iperf3 DEFAULT_UDP_BLKSIZE). */
#define H2_IPERF_DEFAULT_UDP_BLOCK_LEN 1460u
/** Default block size for SCTP streams (iperf3 DEFAULT_SCTP_BLKSIZE). */
#define H2_IPERF_DEFAULT_SCTP_BLOCK_LEN (64u * 1024u)
/** Default target bit rate for UDP streams (iperf3 UDP_RATE). */
#define H2_IPERF_DEFAULT_UDP_BITRATE (1024u * 1024u)
/** Default SCTP packet budget carried inside one UDP datagram. */
#define H2_IPERF_DEFAULT_SCTP_PACKET_SIZE 1280u
/** Default test duration in milliseconds. */
#define H2_IPERF_DEFAULT_DURATION_MS 10000u

/** Data stream transport. */
typedef enum h2_iperf_protocol {
    H2_IPERF_PROTOCOL_TCP = 0,
    H2_IPERF_PROTOCOL_UDP = 1,
    H2_IPERF_PROTOCOL_SCTP = 2,
} h2_iperf_protocol_t;

/** Borrowed PAL capabilities shared by the client and the server. */
typedef struct h2_iperf_config {
    /** Required allocator for buffers and server state. */
    const h2_pal_mem_api_t *mem;
    /** Required network provider (TCP, UDP, and for the server tcp_listen). */
    const h2_pal_net_api_t *net;
    /** Required monotonic clock and sleep source. */
    const h2_pal_time_api_t *time;
    /** Optional random source; required for SCTP, otherwise a fallback is used. */
    const h2_pal_crypto_api_t *crypto;
    /** Optional SCTP provider; required for H2_IPERF_PROTOCOL_SCTP. */
    const h2_pal_sctp_api_t *sctp;
    /** Optional diagnostics sink. */
    const h2_pal_log_api_t *log;
} h2_iperf_config_t;

/** Per-endpoint measurement of one stream. */
typedef struct h2_iperf_stream_stats {
    /** Payload bytes sent or received by this endpoint in its role. */
    uint64_t bytes;
    /** UDP datagrams or SCTP messages; zero for TCP. */
    uint64_t packets;
    /** Receiver-side loss count for UDP; -1 when unknown. */
    int64_t lost_packets;
    /** Receiver-side out-of-order count for UDP. */
    uint64_t out_of_order;
    /** Receiver-side RFC 1889 jitter estimate for UDP in milliseconds. */
    double jitter_ms;
    /** Sender-side TCP retransmits; -1 when unknown. */
    int32_t retransmits;
    /** Measured transfer interval in milliseconds. */
    uint32_t duration_ms;
} h2_iperf_stream_stats_t;

/** Outcome of one completed test as seen by this endpoint. */
typedef struct h2_iperf_result {
    h2_iperf_protocol_t protocol;
    /** True when the server sent and the client received. */
    bool reverse;
    /** What this endpoint measured itself. */
    h2_iperf_stream_stats_t local;
    /** What the peer reported during the result exchange. */
    h2_iperf_stream_stats_t remote;
    /** Test cookie negotiated on the control connection. */
    char cookie[H2_IPERF_COOKIE_SIZE];
} h2_iperf_result_t;

/** Client run parameters; zero fields select iperf3 defaults. */
typedef struct h2_iperf_client_params {
    /** Optional host name resolved through the PAL resolver. */
    const char *server_host;
    /** Server address used when `server_host` is NULL; port may be zero. */
    h2_pal_net_addr_t server_addr;
    /** Control and data port; zero selects H2_IPERF_DEFAULT_PORT. */
    uint16_t port;
    h2_iperf_protocol_t protocol;
    /** Server sends, client receives (`iperf3 -R`). */
    bool reverse;
    /** Test duration (`-t`); zero selects ten seconds unless `bytes` is set. */
    uint32_t duration_ms;
    /** Stop after this many payload bytes (`-n`); zero uses the duration. */
    uint64_t bytes;
    /** Block or datagram length (`-l`); zero selects the protocol default. */
    uint32_t block_len;
    /** Target bit rate (`-b`); zero means unlimited for TCP/SCTP, 1 Mbit/s for UDP. */
    uint64_t bitrate_bps;
    /** UDP port carrying SCTP packets; zero selects 9899. */
    uint16_t sctp_udp_port;
    /** SCTP packet budget per datagram; zero selects 1280. */
    uint16_t sctp_packet_size;
    /** Control connection setup timeout; zero selects five seconds. */
    uint32_t connect_timeout_ms;
    /** Timeout for each control message; zero selects thirty seconds. */
    uint32_t control_timeout_ms;
} h2_iperf_client_params_t;

/** Server parameters; zero fields select iperf3 defaults. */
typedef struct h2_iperf_server_params {
    /** Address family to listen on; zero selects IPv4. */
    h2_pal_net_family_t family;
    /** Optional bind constraint for the listeners. */
    const h2_pal_net_bind_t *bind;
    /** Control and data port; zero selects H2_IPERF_DEFAULT_PORT. */
    uint16_t port;
    /** True to ignore `port` and let the platform choose a free port. */
    bool ephemeral_port;
    /** UDP port receiving encapsulated SCTP; zero selects 9899. */
    uint16_t sctp_udp_port;
    /** True to let the platform choose the SCTP encapsulation port. */
    bool ephemeral_sctp_udp_port;
    /** SCTP packet budget per datagram; zero selects 1280. */
    uint16_t sctp_packet_size;
    /** Timeout for each control message; zero selects thirty seconds. */
    uint32_t control_timeout_ms;
} h2_iperf_server_params_t;

/** Opaque server instance owning its listeners. */
typedef struct h2_iperf_server h2_iperf_server_t;

/**
 * Runs one complete iperf3 client test and blocks until it finishes.
 *
 * Returns `H2_PAL_OK` with `out_result` filled when the server completed the
 * exchange. `H2_PAL_ERR_BUSY` reports an iperf3 ACCESS_DENIED response and
 * `H2_PAL_ERR_UNSUPPORTED` a protocol the configuration cannot provide.
 */
h2_pal_result_t h2_iperf_client_run(
    const h2_iperf_config_t *config,
    const h2_iperf_client_params_t *params,
    h2_iperf_result_t *out_result);

/** Creates a server and binds its control listener (and SCTP UDP socket). */
h2_pal_result_t h2_iperf_server_create(
    const h2_iperf_config_t *config,
    const h2_iperf_server_params_t *params,
    h2_iperf_server_t **out_server);

/** Returns the bound control/data port, or zero for a NULL server. */
uint16_t h2_iperf_server_port(const h2_iperf_server_t *server);

/** Returns the bound SCTP encapsulation UDP port, or zero when absent. */
uint16_t h2_iperf_server_sctp_udp_port(const h2_iperf_server_t *server);

/**
 * Serves exactly one client test.
 *
 * Waits up to `accept_timeout_ms` for a control connection and returns
 * `H2_PAL_ERR_TIMEOUT` when none arrives. Any other result reflects the test
 * outcome; `out_result` is valid only on `H2_PAL_OK`.
 */
h2_pal_result_t h2_iperf_server_run_once(
    h2_iperf_server_t *server,
    uint32_t accept_timeout_ms,
    h2_iperf_result_t *out_result);

/** Closes the listeners and releases the server; NULL is a no-op. */
void h2_iperf_server_destroy(h2_iperf_server_t **server);

/** Computes throughput in bits per second from one stats record. */
static inline uint64_t h2_iperf_stats_bits_per_second(
    const h2_iperf_stream_stats_t *stats) {
    if (stats == NULL || stats->duration_ms == 0u) {
        return 0u;
    }
    return (stats->bytes * 8000u) / stats->duration_ms;
}

#ifdef __cplusplus
}
#endif

#endif
