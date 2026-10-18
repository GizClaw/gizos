#include "h2_iperf_e2e.h"
#include "h2_iperf_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct server_thread {
    h2_iperf_server_t *server;
    size_t runs;
    size_t completed;
} server_thread_t;

static void *server_main(void *arg) {
    server_thread_t *thread = arg;
    for (size_t i = 0u; i < thread->runs; ++i) {
        h2_iperf_result_t result;
        if (h2_iperf_server_run_once(thread->server, 10000u, &result) !=
            H2_PAL_OK) {
            break;
        }
        ++thread->completed;
    }
    return NULL;
}

static const h2_iperf_e2e_case_t s_cases[] = {
    {"tcp_tx", H2_IPERF_PROTOCOL_TCP, false, 0u, 0u, 0u},
    {"udp_rx_2m", H2_IPERF_PROTOCOL_UDP, true, 0u, 2000000u, 0u},
    {"sctp_tx_1200", H2_IPERF_PROTOCOL_SCTP, false, 4096u, 0u, 1200u},
    /* A second association on the same encapsulation socket must not be
     * seeded by late packets of the previous one. */
    {"sctp_rx_1200", H2_IPERF_PROTOCOL_SCTP, true, 4096u, 0u, 1200u},
    {"sctp_tx_1400", H2_IPERF_PROTOCOL_SCTP, false, 4096u, 0u, 1400u},
};

typedef struct ledger_capture {
    char message[512];
    unsigned records;
} ledger_capture_t;

static int capture_ledger(void *user, h2_pal_log_level_t level,
                          const char *scope, const char *message) {
    ledger_capture_t *capture = user;
    assert(level == H2_PAL_LOG_INFO);
    assert(strcmp(scope, "iperf_e2e") == 0);
    const size_t length = strlen(message);
    assert(length < sizeof(capture->message));
    memcpy(capture->message, message, length + 1u);
    ++capture->records;
    return H2_PAL_OK;
}

static void test_case_ledger_preserves_long_records(void) {
    static const h2_pal_log_vtable_t vtable = {.write = capture_ledger};
    ledger_capture_t capture = {0};
    const h2_pal_log_api_t log = {.user = &capture, .vtable = &vtable};
    h2_iperf_e2e_case_t test_case = {
        .id = "udp_tx_40m", .protocol = H2_IPERF_PROTOCOL_UDP,
        .block_len = 1200u, .bitrate_bps = 40000000u};
    h2_iperf_e2e_case_result_t result = {
        .id = "udp_tx_40m", .rc = H2_PAL_OK,
        .receiver_bps = 27365171u, .sender_bps = 37324800u,
        .packets = 19439u, .lost_packets = 4887,
        .jitter_us = 334u, .duration_ms = 6237u};
    static const char expected_dual[] =
        "H2_IPERF_E2E_CASE {\"v\":1,\"target\":\"zero_esp_v3_0-m46-f4-r1\","
        "\"id\":\"udp_tx_40m\",\"proto\":\"udp\",\"dir\":\"tx\",\"block\":1200,"
        "\"bitrate\":40000000,\"pkt\":0,\"rc\":0,\"rx_bps\":27365171,"
        "\"tx_bps\":37324800,\"packets\":19439,\"lost\":4887,"
        "\"jitter_us\":334,\"ms\":6237,\"result\":\"pass\"}";
    h2_iperf_e2e_log_case(&log, "zero_esp_v3_0-m46-f4-r1", &test_case,
                         &result);
    assert(strlen(expected_dual) == H2_PAL_LOG_MESSAGE_MAX);
    assert(capture.records == 1u);
    assert(capture.message[strlen(capture.message) - 1u] == '}');
    assert(strcmp(capture.message, expected_dual) == 0);

    test_case = (h2_iperf_e2e_case_t){
        .id = "0123456789012345678901234567890",
        .protocol = H2_IPERF_PROTOCOL_SCTP, .reverse = true,
        .block_len = UINT32_MAX, .bitrate_bps = UINT64_MAX,
        .sctp_packet_size = UINT16_MAX};
    result = (h2_iperf_e2e_case_result_t){
        .id = "0123456789012345678901234567890", .rc = INT32_MIN,
        .receiver_bps = UINT64_MAX, .sender_bps = UINT64_MAX,
        .packets = UINT64_MAX, .lost_packets = INT64_MIN,
        .jitter_us = UINT32_MAX, .duration_ms = UINT32_MAX};
    static const char expected_max[] =
        "H2_IPERF_E2E_CASE {\"v\":1,\"target\":\"0123456789012345678901234567890\","
        "\"id\":\"0123456789012345678901234567890\",\"proto\":\"sctp\",\"dir\":\"rx\","
        "\"block\":4294967295,\"bitrate\":18446744073709551615,\"pkt\":65535,"
        "\"rc\":-2147483648,\"rx_bps\":18446744073709551615,"
        "\"tx_bps\":18446744073709551615,\"packets\":18446744073709551615,"
        "\"lost\":-9223372036854775808,\"jitter_us\":4294967295,\"ms\":4294967295,"
        "\"result\":\"fail\"}";
    h2_iperf_e2e_log_case(&log, test_case.id, &test_case, &result);
    assert(strlen(test_case.id) == H2_IPERF_E2E_CASE_ID_MAX);
    assert(strlen(test_case.id) == H2_IPERF_E2E_TARGET_MAX);
    assert(strlen(expected_max) == 386u);
    assert(capture.records == 2u);
    assert(strcmp(capture.message, expected_max) == 0);
}

static void test_rejects_invalid_config(void) {
    h2_iperf_e2e_report_t report;
    h2_iperf_e2e_config_t config;
    memset(&config, 0, sizeof(config));
    assert(h2_iperf_e2e_run(NULL, &report) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_iperf_e2e_run(&config, &report) == H2_PAL_ERR_INVALID_ARG);

    h2_iperf_test_env_t env;
    h2_iperf_test_env_init(&env, false);
    config.pal = env.config;
    config.target = "host";
    config.server_addr = h2_iperf_test_loopback(1u);
    /* A non-empty case list with a zero count is inconsistent. */
    config.cases = s_cases;
    config.case_count = 0u;
    assert(h2_iperf_e2e_run(&config, &report) == H2_PAL_ERR_INVALID_ARG);
    config.case_count = 1u;
    assert(h2_iperf_e2e_run(&config, NULL) == H2_PAL_ERR_INVALID_ARG);
    /* Ledger identifiers are bounded tokens, never escaped. */
    config.target = "amoled\"x";
    assert(h2_iperf_e2e_run(&config, &report) == H2_PAL_ERR_INVALID_ARG);
    config.target = "0123456789012345678901234567890123";
    assert(h2_iperf_e2e_run(&config, &report) == H2_PAL_ERR_INVALID_ARG);
    config.target = "host-1.local";
    const h2_iperf_e2e_case_t bad_id[] = {
        {"tcp tx", H2_IPERF_PROTOCOL_TCP, false, 0u, 0u, 0u},
    };
    config.cases = bad_id;
    assert(h2_iperf_e2e_run(&config, &report) == H2_PAL_ERR_INVALID_ARG);
    const h2_iperf_e2e_case_t long_id[] = {
        {"0123456789012345678901234567890123", H2_IPERF_PROTOCOL_TCP, false, 0u,
         0u, 0u},
    };
    config.cases = long_id;
    assert(h2_iperf_e2e_run(&config, &report) == H2_PAL_ERR_INVALID_ARG);
    h2_iperf_test_env_deinit(&env);
}

static void test_unsupported_sctp_case_is_reported_not_fatal(void) {
    h2_iperf_test_env_t env;
    h2_iperf_test_env_init(&env, false);
    h2_iperf_e2e_config_t config;
    memset(&config, 0, sizeof(config));
    config.pal = env.config;
    config.target = "host";
    config.server_addr = h2_iperf_test_loopback(1u);
    config.cases = &s_cases[2];
    config.case_count = 1u;
    config.duration_ms = 100u;
    config.settle_ms = 1u;
    h2_iperf_e2e_report_t report;
    assert(h2_iperf_e2e_run(&config, &report) == H2_PAL_ERR_IO);
    assert(report.total == 1u && report.passed == 0u);
    h2_iperf_test_env_deinit(&env);
}

static void test_loopback_matrix(void) {
    h2_iperf_test_env_t server_env;
    h2_iperf_test_env_t client_env;
    h2_iperf_test_env_init(&server_env, true);
    h2_iperf_test_env_init(&client_env, true);

    h2_iperf_server_params_t server_params;
    memset(&server_params, 0, sizeof(server_params));
    server_params.ephemeral_port = true;
    server_params.ephemeral_sctp_udp_port = true;
    server_thread_t thread = {.runs = sizeof(s_cases) / sizeof(s_cases[0])};
    assert(h2_iperf_server_create(&server_env.config, &server_params,
                                  &thread.server) == H2_PAL_OK);
    pthread_t handle;
    assert(pthread_create(&handle, NULL, server_main, &thread) == 0);

    h2_iperf_e2e_config_t config;
    memset(&config, 0, sizeof(config));
    config.pal = client_env.config;
    config.target = "host";
    config.server_addr =
        h2_iperf_test_loopback(h2_iperf_server_port(thread.server));
    config.port = h2_iperf_server_port(thread.server);
    config.sctp_udp_port = h2_iperf_server_sctp_udp_port(thread.server);
    config.duration_ms = 400u;
    config.settle_ms = 50u;
    config.cases = s_cases;
    config.case_count = thread.runs;
    h2_iperf_e2e_report_t report;
    const h2_pal_result_t rc = h2_iperf_e2e_run(&config, &report);
    assert(pthread_join(handle, NULL) == 0);
    assert(rc == H2_PAL_OK);
    assert(report.total == thread.runs && report.passed == thread.runs);
    assert(thread.completed == thread.runs);
    assert(report.tcp_tx_bps > 0u);
    assert(report.udp_rx_bps > 0u);
    assert(report.sctp_tx_bps > 0u);
    assert(report.sctp_rx_bps > 0u);
    assert(report.tcp_rx_bps == 0u && report.udp_tx_bps == 0u);

    h2_iperf_server_destroy(&thread.server);
    h2_iperf_test_env_deinit(&client_env);
    h2_iperf_test_env_deinit(&server_env);
}

static void test_default_matrix_is_well_formed(void) {
    assert(h2_iperf_e2e_default_case_count >= 6u);
    for (size_t i = 0u; i < h2_iperf_e2e_default_case_count; ++i) {
        const h2_iperf_e2e_case_t *test_case = &h2_iperf_e2e_default_cases[i];
        assert(test_case->id != NULL);
        assert(strlen(test_case->id) <= H2_IPERF_E2E_CASE_ID_MAX);
        for (size_t j = 0u; j < i; ++j) {
            assert(strcmp(test_case->id, h2_iperf_e2e_default_cases[j].id) != 0);
        }
    }
}

int main(void) {
    test_case_ledger_preserves_long_records();
    test_default_matrix_is_well_formed();
    test_rejects_invalid_config();
    test_unsupported_sctp_case_is_reported_not_fatal();
    test_loopback_matrix();
    printf("iperf_e2e_test PASS\n");
    return 0;
}
