#ifndef H2_IPERF_E2E_H
#define H2_IPERF_E2E_H

/**
 * @file h2_iperf_e2e.h
 * @brief Portable iperf3 throughput matrix for Desktop and device launchers.
 *
 * The App runs a fixed, ordered list of iperf3 client cases against one
 * operator-provided server through the PAL-only `libs/iperf` client and
 * writes one machine-readable line per case through the Log PAL. It owns case IDs, ordering,
 * per-case deadlines and the non-fail-fast aggregation; launchers own the
 * Runtime assembly, the server endpoint and Wi-Fi lifecycle.
 */

#include "h2_iperf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Longest case ID and target label, excluding the terminator. Both are
 * written verbatim into JSON ledger strings, so they must consist only of
 * ASCII letters, digits, `_`, `-` and `.`.
 */
#define H2_IPERF_E2E_CASE_ID_MAX 31u
#define H2_IPERF_E2E_TARGET_MAX 31u

/** One client run against the configured server. */
typedef struct h2_iperf_e2e_case {
    /** Stable case identifier for the ledger; bounded by
     * H2_IPERF_E2E_CASE_ID_MAX and limited to the ledger token alphabet. */
    const char *id;
    h2_iperf_protocol_t protocol;
    /** True when the server sends and this endpoint receives. */
    bool reverse;
    /** Block or datagram length; zero selects the iperf3 default. */
    uint32_t block_len;
    /** Target bit rate; zero is unlimited for TCP/SCTP and 1 Mbit/s for UDP. */
    uint64_t bitrate_bps;
    /** SCTP packet budget per UDP datagram; zero selects 1280. */
    uint16_t sctp_packet_size;
} h2_iperf_e2e_case_t;

typedef void (*h2_iperf_e2e_checkpoint_fn)(void *user, const char *name);

/** Launcher-owned run configuration. */
typedef struct h2_iperf_e2e_config {
    /** Borrowed PAL views; `sctp` may be NULL when no SCTP case is selected. */
    h2_iperf_config_t pal;
    /** Launcher label in every ledger line, for example "amoled"; bounded by
     * H2_IPERF_E2E_TARGET_MAX and limited to the ledger token alphabet. */
    const char *target;
    /** Optional host name resolved through the PAL resolver. */
    const char *server_host;
    /** Server address used when `server_host` is NULL. */
    h2_pal_net_addr_t server_addr;
    /** Control and data port; zero selects 5201. */
    uint16_t port;
    /** UDP port carrying SCTP packets; zero selects 9899. */
    uint16_t sctp_udp_port;
    /** Duration of every case; zero selects five seconds. */
    uint32_t duration_ms;
    /** Idle gap between cases; zero selects 500 ms. */
    uint32_t settle_ms;
    /** Case list; NULL selects h2_iperf_e2e_default_cases. */
    const h2_iperf_e2e_case_t *cases;
    size_t case_count;
    /** Optional memory/telemetry hook invoked before and after each case. */
    h2_iperf_e2e_checkpoint_fn checkpoint;
    void *checkpoint_user;
} h2_iperf_e2e_config_t;

/** Per-case ledger entry. */
typedef struct h2_iperf_e2e_case_result {
    char id[H2_IPERF_E2E_CASE_ID_MAX + 1u];
    h2_pal_result_t rc;
    /** Bits per second measured by the receiving endpoint. */
    uint64_t receiver_bps;
    /** Bits per second measured by the sending endpoint. */
    uint64_t sender_bps;
    /** Receiver-side lost datagrams for UDP; -1 when unknown. */
    int64_t lost_packets;
    /** Receiver-side datagrams or messages; zero for TCP. */
    uint64_t packets;
    /** Receiver-side RFC 1889 jitter in microseconds for UDP. */
    uint32_t jitter_us;
    /** Wall-clock case duration measured by this endpoint. */
    uint32_t duration_ms;
} h2_iperf_e2e_case_result_t;

/** Aggregate report over the whole matrix. */
typedef struct h2_iperf_e2e_report {
    unsigned total;
    unsigned passed;
    /** Best receiver-side throughput per transport and direction. */
    uint64_t udp_tx_bps;
    uint64_t udp_rx_bps;
    uint64_t sctp_tx_bps;
    uint64_t sctp_rx_bps;
    uint64_t tcp_tx_bps;
    uint64_t tcp_rx_bps;
} h2_iperf_e2e_report_t;

/** Default matrix: TCP reference, UDP sweeps, SCTP at the H2Peer MTU. */
extern const h2_iperf_e2e_case_t h2_iperf_e2e_default_cases[];
extern const size_t h2_iperf_e2e_default_case_count;

/**
 * Runs the whole matrix and returns H2_PAL_OK only when every case passed.
 *
 * Missing PAL views, an invalid target label or an invalid case list return
 * H2_PAL_ERR_INVALID_ARG before any traffic; an SCTP case without `pal.sctp` reports
 * H2_PAL_ERR_UNSUPPORTED for that case and the run continues.
 */
h2_pal_result_t h2_iperf_e2e_run(
    const h2_iperf_e2e_config_t *config,
    h2_iperf_e2e_report_t *out_report);

/** Writes one ledger line for a completed case through the Log PAL. */
void h2_iperf_e2e_log_case(
    const h2_pal_log_api_t *log,
    const char *target,
    const h2_iperf_e2e_case_t *test_case,
    const h2_iperf_e2e_case_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
