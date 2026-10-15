#include "h2_iperf_client_app.h"

#include <stdio.h>
#include <string.h>

const char h2_iperf_client_app_task_name[] = "iperf-client/run";

static const h2_iperf_e2e_case_t cases[] = {
    {"tcp_tx", H2_IPERF_PROTOCOL_TCP, false, 16384u, 0u, 0u},
    {"tcp_rx", H2_IPERF_PROTOCOL_TCP, true, 16384u, 0u, 0u},
    {"udp_tx_5m", H2_IPERF_PROTOCOL_UDP, false, 1200u, 5000000u, 0u},
    {"udp_rx_5m", H2_IPERF_PROTOCOL_UDP, true, 1200u, 5000000u, 0u},
    {"udp_tx_10m", H2_IPERF_PROTOCOL_UDP, false, 1200u, 10000000u, 0u},
    {"udp_rx_10m", H2_IPERF_PROTOCOL_UDP, true, 1200u, 10000000u, 0u},
    {"udp_tx_20m", H2_IPERF_PROTOCOL_UDP, false, 1200u, 20000000u, 0u},
    {"udp_rx_20m", H2_IPERF_PROTOCOL_UDP, true, 1200u, 20000000u, 0u},
    {"udp_tx_40m", H2_IPERF_PROTOCOL_UDP, false, 1200u, 40000000u, 0u},
    {"udp_rx_40m", H2_IPERF_PROTOCOL_UDP, true, 1200u, 40000000u, 0u},
};

h2_pal_result_t
h2_iperf_client_app_run(h2_runtime_t *runtime,
                        const h2_iperf_client_app_config_t *config,
                        h2_iperf_client_app_report_t *out_report) {
  if (out_report != NULL)
    memset(out_report, 0, sizeof(*out_report));
  if (runtime == NULL || config == NULL || out_report == NULL ||
      runtime->mem == NULL || runtime->net == NULL || runtime->time == NULL ||
      (config->mode != H2_IPERF_CLIENT_APP_IPV4 &&
       config->mode != H2_IPERF_CLIENT_APP_IPV6 &&
       config->mode != H2_IPERF_CLIENT_APP_DUAL) ||
      config->rounds > 10u ||
      (config->mode != H2_IPERF_CLIENT_APP_IPV6 &&
       config->ipv4.family != H2_PAL_NET_FAMILY_IPV4) ||
      (config->mode != H2_IPERF_CLIENT_APP_IPV4 &&
       config->ipv6.family != H2_PAL_NET_FAMILY_IPV6))
    return H2_PAL_ERR_INVALID_ARG;
  const char *board = config->target == NULL ? "client" : config->target;
  size_t length = strlen(board);
  if (length == 0u || length > 16u)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < length; ++i)
    if (!((board[i] >= 'a' && board[i] <= 'z') ||
          (board[i] >= '0' && board[i] <= '9') || board[i] == '-' ||
          board[i] == '_'))
      return H2_PAL_ERR_INVALID_ARG;
  const unsigned rounds = config->rounds == 0u ? 3u : config->rounds;
  for (unsigned family = 4u; family <= 6u; family += 2u) {
    if ((family == 4u && config->mode == H2_IPERF_CLIENT_APP_IPV6) ||
        (family == 6u && config->mode == H2_IPERF_CLIENT_APP_IPV4))
      continue;
    for (unsigned round = 1u; round <= rounds; ++round) {
      char target[H2_IPERF_E2E_TARGET_MAX + 1u];
      (void)snprintf(target, sizeof(target), "%s-m%u-f%u-r%u", board,
                     (unsigned)config->mode, family, round);
      const h2_iperf_e2e_config_t matrix = {
          .pal = {.mem = runtime->mem,
                  .net = runtime->net,
                  .time = runtime->time,
                  .crypto = runtime->crypto,
                  .log = runtime->log,
                  .max_json_len = 4096u},
          .target = target,
          .server_addr = family == 4u ? config->ipv4 : config->ipv6,
          .port = config->port == 0u ? H2_IPERF_DEFAULT_PORT : config->port,
          .duration_ms = config->duration_ms,
          .settle_ms = config->settle_ms,
          .cases = cases,
          .case_count = sizeof(cases) / sizeof(cases[0]),
          .checkpoint = config->checkpoint,
          .checkpoint_user = config->checkpoint_user};
      h2_iperf_e2e_report_t report = {0};
      int rc = h2_iperf_e2e_run(&matrix, &report);
      if (rc == H2_PAL_ERR_INVALID_ARG)
        return rc;
      out_report->total += report.total;
      out_report->passed += report.passed;
    }
  }
  return out_report->total == out_report->passed ? H2_PAL_OK : H2_PAL_ERR_IO;
}
