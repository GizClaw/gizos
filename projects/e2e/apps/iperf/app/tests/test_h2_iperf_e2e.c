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
    test_default_matrix_is_well_formed();
    test_rejects_invalid_config();
    test_unsupported_sctp_case_is_reported_not_fatal();
    test_loopback_matrix();
    printf("iperf_e2e_test PASS\n");
    return 0;
}
