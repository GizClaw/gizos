#include "h2_iperf.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void) {
    h2_iperf_stream_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    assert(h2_iperf_stats_bits_per_second(&stats) == 0u);
    assert(h2_iperf_stats_bits_per_second(NULL) == 0u);
    stats.bytes = 1000u;
    stats.duration_ms = 1000u;
    assert(h2_iperf_stats_bits_per_second(&stats) == 8000u);

    h2_iperf_result_t result;
    assert(h2_iperf_client_run(NULL, NULL, &result) == H2_PAL_ERR_INVALID_ARG);
    h2_iperf_server_t *server = NULL;
    assert(h2_iperf_server_create(NULL, NULL, &server) == H2_PAL_ERR_INVALID_ARG);
    assert(server == NULL);
    assert(h2_iperf_server_port(NULL) == 0u);
    assert(h2_iperf_server_sctp_udp_port(NULL) == 0u);
    assert(h2_iperf_server_run_once(NULL, 0u, &result) == H2_PAL_ERR_INVALID_ARG);
    h2_iperf_server_destroy(&server);
    h2_iperf_server_destroy(NULL);
    assert(H2_IPERF_COOKIE_SIZE == 37u);
    assert(H2_IPERF_DEFAULT_PORT == 5201u);
    return 0;
}
