#include "mobile_runner.h"
int h2_net_tls_mobile_run(h2_runtime_config_t config, const char *host,
                          uint16_t port, const char *session, const uint8_t *ca,
                          size_t ca_len, const uint8_t *wrong, size_t wrong_len,
                          const char *dns_host, const char *dns_ip,
                          h2_net_tls_result_t *result) {
  h2_runtime_t *runtime = NULL;
  int rc = h2_runtime_init(&config, &runtime);
  if (rc == H2_PAL_OK) {
    h2_net_tls_fixture_client_t client = {
        .runtime = runtime, .host = host, .session = session, .port = port};
    h2_net_tls_config_t fixture = {.runtime = runtime,
                                   .host = host,
                                   .session = session,
                                   .root_ca = ca,
                                   .root_ca_len = ca_len,
                                   .wrong_ca = wrong,
                                   .wrong_ca_len = wrong_len,
                                   .dns_host = dns_host,
                                   .server_name = "pal-net-tls.test",
                                   .prepare = h2_net_tls_fixture_prepare,
                                   .verify = h2_net_tls_fixture_verify,
                                   .fixture_user = &client,
                                   .report = h2_net_tls_report,
                                   .case_timeout_ms = 30000u,
                                   .multicast_supported = 1};
    rc = h2_net_tls_parse_ipv4(dns_ip, &fixture.dns_expected);
    if (rc == H2_PAL_OK)
      rc = h2_pal_net_tls_e2e_run(&fixture, result);
    h2_runtime_deinit(runtime);
  }
  return rc;
}
