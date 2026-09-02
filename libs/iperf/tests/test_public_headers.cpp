#include "h2_iperf.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>

int main() {
    h2_iperf_client_params_t params{};
    params.protocol = H2_IPERF_PROTOCOL_SCTP;
    params.port = H2_IPERF_DEFAULT_PORT;
    h2_iperf_result_t result{};
    assert(h2_iperf_client_run(nullptr, &params, &result) == H2_PAL_ERR_INVALID_ARG);
    h2_iperf_stream_stats_t stats{};
    stats.bytes = 4000u;
    stats.duration_ms = 2000u;
    assert(h2_iperf_stats_bits_per_second(&stats) == 16000u);
    h2_iperf_server_t *server = nullptr;
    h2_iperf_server_destroy(&server);
    return 0;
}
