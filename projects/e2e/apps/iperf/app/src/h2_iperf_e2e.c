#include "h2_iperf_e2e.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define H2_IPERF_E2E_LOG_SCOPE "iperf_e2e"

#define H2_IPERF_E2E_DEFAULT_DURATION_MS 5000u
#define H2_IPERF_E2E_DEFAULT_SETTLE_MS 500u
/* Matches the H2Peer portable provider SCTP packet budget. */
#define H2_IPERF_E2E_H2PEER_SCTP_PACKET 1200u
#define H2_IPERF_E2E_MBIT(value) ((uint64_t)(value) * 1000000u)

const h2_iperf_e2e_case_t h2_iperf_e2e_default_cases[] = {
    {"tcp_tx", H2_IPERF_PROTOCOL_TCP, false, 0u, 0u, 0u},
    {"tcp_rx", H2_IPERF_PROTOCOL_TCP, true, 0u, 0u, 0u},
    {"udp_tx_10m", H2_IPERF_PROTOCOL_UDP, false, 0u, H2_IPERF_E2E_MBIT(10), 0u},
    {"udp_tx_20m", H2_IPERF_PROTOCOL_UDP, false, 0u, H2_IPERF_E2E_MBIT(20), 0u},
    {"udp_tx_40m", H2_IPERF_PROTOCOL_UDP, false, 0u, H2_IPERF_E2E_MBIT(40), 0u},
    {"udp_tx_20m_1200", H2_IPERF_PROTOCOL_UDP, false, 1200u,
     H2_IPERF_E2E_MBIT(20), 0u},
    {"udp_rx_10m", H2_IPERF_PROTOCOL_UDP, true, 0u, H2_IPERF_E2E_MBIT(10), 0u},
    {"udp_rx_20m", H2_IPERF_PROTOCOL_UDP, true, 0u, H2_IPERF_E2E_MBIT(20), 0u},
    {"udp_rx_40m", H2_IPERF_PROTOCOL_UDP, true, 0u, H2_IPERF_E2E_MBIT(40), 0u},
    {"udp_rx_20m_1200", H2_IPERF_PROTOCOL_UDP, true, 1200u,
     H2_IPERF_E2E_MBIT(20), 0u},
    {"sctp_tx_1200", H2_IPERF_PROTOCOL_SCTP, false, 8u * 1024u, 0u,
     H2_IPERF_E2E_H2PEER_SCTP_PACKET},
    {"sctp_rx_1200", H2_IPERF_PROTOCOL_SCTP, true, 8u * 1024u, 0u,
     H2_IPERF_E2E_H2PEER_SCTP_PACKET},
    {"sctp_tx_1400", H2_IPERF_PROTOCOL_SCTP, false, 8u * 1024u, 0u, 1400u},
    {"sctp_rx_1400", H2_IPERF_PROTOCOL_SCTP, true, 8u * 1024u, 0u, 1400u},
};

const size_t h2_iperf_e2e_default_case_count =
    sizeof(h2_iperf_e2e_default_cases) / sizeof(h2_iperf_e2e_default_cases[0]);

static const char *protocol_name(h2_iperf_protocol_t protocol) {
    switch (protocol) {
    case H2_IPERF_PROTOCOL_TCP:
        return "tcp";
    case H2_IPERF_PROTOCOL_UDP:
        return "udp";
    case H2_IPERF_PROTOCOL_SCTP:
        return "sctp";
    default:
        return "unknown";
    }
}

/* Ledger identifiers are emitted verbatim inside JSON strings, so they are
 * restricted to a bounded token alphabet instead of being escaped. */
static bool identifier_is_valid(const char *text, size_t max_len) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    for (size_t i = 0u; text[i] != '\0'; ++i) {
        if (i >= max_len) {
            return false;
        }
        const char c = text[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                        c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool case_is_valid(const h2_iperf_e2e_case_t *test_case) {
    if (test_case == NULL ||
        !identifier_is_valid(test_case->id, H2_IPERF_E2E_CASE_ID_MAX)) {
        return false;
    }
    return test_case->protocol == H2_IPERF_PROTOCOL_TCP ||
           test_case->protocol == H2_IPERF_PROTOCOL_UDP ||
           test_case->protocol == H2_IPERF_PROTOCOL_SCTP;
}

static bool config_is_valid(const h2_iperf_e2e_config_t *config) {
    if (config == NULL || config->pal.mem == NULL || config->pal.net == NULL ||
        config->pal.time == NULL ||
        !identifier_is_valid(config->target, H2_IPERF_E2E_TARGET_MAX)) {
        return false;
    }
    if (config->server_host == NULL &&
        config->server_addr.family != H2_PAL_NET_FAMILY_IPV4 &&
        config->server_addr.family != H2_PAL_NET_FAMILY_IPV6) {
        return false;
    }
    if ((config->cases == NULL) != (config->case_count == 0u)) {
        return false;
    }
    const h2_iperf_e2e_case_t *cases =
        config->cases == NULL ? h2_iperf_e2e_default_cases : config->cases;
    const size_t count = config->cases == NULL
                             ? h2_iperf_e2e_default_case_count
                             : config->case_count;
    for (size_t i = 0u; i < count; ++i) {
        if (!case_is_valid(&cases[i])) {
            return false;
        }
    }
    return true;
}

static void checkpoint(const h2_iperf_e2e_config_t *config, const char *name) {
    if (config->checkpoint != NULL) {
        config->checkpoint(config->checkpoint_user, name);
    }
}

static uint64_t stats_bps(const h2_iperf_stream_stats_t *stats) {
    return h2_iperf_stats_bits_per_second(stats);
}

static void record_best(uint64_t *slot, uint64_t value) {
    if (value > *slot) {
        *slot = value;
    }
}

/* Every line goes through the Log PAL; portable code never touches newlib
 * stdout state. Lines are bounded by H2_PAL_LOG_MESSAGE_MAX. */
static void emit(const h2_pal_log_api_t *log, const char *format, ...) {
    char message[H2_PAL_LOG_MESSAGE_MAX];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    (void)h2_pal_log_write(log, H2_PAL_LOG_INFO, H2_IPERF_E2E_LOG_SCOPE, message);
}

void h2_iperf_e2e_log_case(
    const h2_pal_log_api_t *log,
    const char *target,
    const h2_iperf_e2e_case_t *test_case,
    const h2_iperf_e2e_case_result_t *result) {
    if (!identifier_is_valid(target, H2_IPERF_E2E_TARGET_MAX) ||
        test_case == NULL || result == NULL ||
        !identifier_is_valid(result->id, H2_IPERF_E2E_CASE_ID_MAX)) {
        return;
    }
    emit(log,
         "H2_IPERF_E2E_CASE {\"v\":1,\"target\":\"%s\",\"id\":\"%s\","
         "\"proto\":\"%s\",\"dir\":\"%s\",\"block\":%u,\"bitrate\":%llu,"
         "\"pkt\":%u,\"rc\":%d,\"rx_bps\":%llu,\"tx_bps\":%llu,"
         "\"packets\":%llu,\"lost\":%lld,\"jitter_us\":%u,\"ms\":%u,"
         "\"result\":\"%s\"}",
         target, result->id, protocol_name(test_case->protocol),
         test_case->reverse ? "rx" : "tx", (unsigned)test_case->block_len,
         (unsigned long long)test_case->bitrate_bps,
         (unsigned)test_case->sctp_packet_size, (int)result->rc,
         (unsigned long long)result->receiver_bps,
         (unsigned long long)result->sender_bps,
         (unsigned long long)result->packets, (long long)result->lost_packets,
         (unsigned)result->jitter_us, (unsigned)result->duration_ms,
         result->rc == H2_PAL_OK ? "pass" : "fail");
}

static void run_case(
    const h2_iperf_e2e_config_t *config,
    const h2_iperf_e2e_case_t *test_case,
    uint32_t duration_ms,
    h2_iperf_e2e_case_result_t *out_result) {
    memset(out_result, 0, sizeof(*out_result));
    strncpy(out_result->id, test_case->id, H2_IPERF_E2E_CASE_ID_MAX);
    out_result->lost_packets = -1;
    if (test_case->protocol == H2_IPERF_PROTOCOL_SCTP &&
        config->pal.sctp == NULL) {
        out_result->rc = H2_PAL_ERR_UNSUPPORTED;
        return;
    }
    h2_iperf_client_params_t params;
    memset(&params, 0, sizeof(params));
    params.server_host = config->server_host;
    params.server_addr = config->server_addr;
    params.port = config->port;
    params.protocol = test_case->protocol;
    params.reverse = test_case->reverse;
    params.duration_ms = duration_ms;
    params.block_len = test_case->block_len;
    params.bitrate_bps = test_case->bitrate_bps;
    params.sctp_udp_port = config->sctp_udp_port;
    params.sctp_packet_size = test_case->sctp_packet_size;

    uint64_t started_ms = 0u;
    uint64_t finished_ms = 0u;
    (void)h2_pal_time_get_monotonic_ms(config->pal.time, &started_ms);
    h2_iperf_result_t result;
    memset(&result, 0, sizeof(result));
    out_result->rc = h2_iperf_client_run(&config->pal, &params, &result);
    (void)h2_pal_time_get_monotonic_ms(config->pal.time, &finished_ms);
    out_result->duration_ms = (uint32_t)(finished_ms - started_ms);
    if (out_result->rc != H2_PAL_OK) {
        return;
    }
    const h2_iperf_stream_stats_t *receiver =
        test_case->reverse ? &result.local : &result.remote;
    const h2_iperf_stream_stats_t *sender =
        test_case->reverse ? &result.remote : &result.local;
    out_result->receiver_bps = stats_bps(receiver);
    out_result->sender_bps = stats_bps(sender);
    out_result->packets = receiver->packets;
    out_result->lost_packets = receiver->lost_packets;
    if (receiver->jitter_ms > 0.0) {
        out_result->jitter_us = (uint32_t)(receiver->jitter_ms * 1000.0);
    }
    if (out_result->receiver_bps == 0u && receiver->bytes == 0u) {
        /* A completed exchange that moved no payload is not a pass. */
        out_result->rc = H2_PAL_ERR_IO;
    }
}

h2_pal_result_t h2_iperf_e2e_run(
    const h2_iperf_e2e_config_t *config,
    h2_iperf_e2e_report_t *out_report) {
    if (out_report != NULL) {
        memset(out_report, 0, sizeof(*out_report));
    }
    if (!config_is_valid(config) || out_report == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_iperf_e2e_case_t *cases =
        config->cases == NULL ? h2_iperf_e2e_default_cases : config->cases;
    const size_t count = config->cases == NULL
                             ? h2_iperf_e2e_default_case_count
                             : config->case_count;
    const uint32_t duration_ms = config->duration_ms == 0u
                                     ? H2_IPERF_E2E_DEFAULT_DURATION_MS
                                     : config->duration_ms;
    const uint32_t settle_ms = config->settle_ms == 0u
                                   ? H2_IPERF_E2E_DEFAULT_SETTLE_MS
                                   : config->settle_ms;
    emit(config->pal.log,
         "H2_IPERF_E2E_START target=%s cases=%u duration_ms=%u port=%u",
         config->target, (unsigned)count, (unsigned)duration_ms,
         (unsigned)(config->port == 0u ? H2_IPERF_DEFAULT_PORT : config->port));
    out_report->total = (unsigned)count;
    for (size_t i = 0u; i < count; ++i) {
        const h2_iperf_e2e_case_t *test_case = &cases[i];
        h2_iperf_e2e_case_result_t result;
        checkpoint(config, "case_start");
        run_case(config, test_case, duration_ms, &result);
        checkpoint(config, "case_end");
        h2_iperf_e2e_log_case(config->pal.log, config->target, test_case,
                              &result);
        if (result.rc == H2_PAL_OK) {
            ++out_report->passed;
            switch (test_case->protocol) {
            case H2_IPERF_PROTOCOL_TCP:
                record_best(test_case->reverse ? &out_report->tcp_rx_bps
                                               : &out_report->tcp_tx_bps,
                            result.receiver_bps);
                break;
            case H2_IPERF_PROTOCOL_UDP:
                record_best(test_case->reverse ? &out_report->udp_rx_bps
                                               : &out_report->udp_tx_bps,
                            result.receiver_bps);
                break;
            case H2_IPERF_PROTOCOL_SCTP:
                record_best(test_case->reverse ? &out_report->sctp_rx_bps
                                               : &out_report->sctp_tx_bps,
                            result.receiver_bps);
                break;
            default:
                break;
            }
        }
        if (i + 1u < count) {
            (void)h2_pal_time_sleep_ms(config->pal.time, settle_ms);
        }
    }
    emit(config->pal.log,
         "H2_IPERF_E2E_SUMMARY target=%s result=%s passed=%u total=%u "
         "tcp_tx_bps=%llu tcp_rx_bps=%llu udp_tx_bps=%llu udp_rx_bps=%llu "
         "sctp_tx_bps=%llu sctp_rx_bps=%llu",
         config->target,
           out_report->passed == out_report->total ? "PASS" : "FAIL",
           out_report->passed, out_report->total,
           (unsigned long long)out_report->tcp_tx_bps,
           (unsigned long long)out_report->tcp_rx_bps,
           (unsigned long long)out_report->udp_tx_bps,
           (unsigned long long)out_report->udp_rx_bps,
           (unsigned long long)out_report->sctp_tx_bps,
           (unsigned long long)out_report->sctp_rx_bps);
    return out_report->passed == out_report->total ? H2_PAL_OK : H2_PAL_ERR_IO;
}
