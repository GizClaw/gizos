#ifndef H2_IPERF_TEST_SUPPORT_H
#define H2_IPERF_TEST_SUPPORT_H

#include "h2_iperf.h"
#include "h2_sctp.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Concrete host PAL providers wired into one iperf configuration. */
typedef struct h2_iperf_test_env {
    h2_iperf_config_t config;
    h2_pal_crypto_api_t crypto_api;
    h2_sctp_t *sctp_provider;
} h2_iperf_test_env_t;

/** Fills `env` with desktop mem/time/log, host net, entropy, and optional SCTP. */
void h2_iperf_test_env_init(h2_iperf_test_env_t *env, bool with_sctp);
void h2_iperf_test_env_deinit(h2_iperf_test_env_t *env);

/** 127.0.0.1 with the given port. */
h2_pal_net_addr_t h2_iperf_test_loopback(uint16_t port);

/** Reserves and releases an ephemeral TCP port for an external process. */
uint16_t h2_iperf_test_free_port(const h2_pal_net_api_t *net);

/** Starts `argv[0]` with stdout/stderr redirected to the test log. */
pid_t h2_iperf_test_spawn(char *const argv[]);
/** Waits up to `timeout_ms`; returns the exit status or -1 on timeout/error. */
int h2_iperf_test_wait(pid_t pid, uint32_t timeout_ms);
void h2_iperf_test_sleep_ms(uint32_t ms);

/** Prints one result record to stdout for the test log. */
void h2_iperf_test_print_result(const char *label, const h2_iperf_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
