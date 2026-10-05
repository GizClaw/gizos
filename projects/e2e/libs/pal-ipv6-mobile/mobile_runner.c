#include "mobile_runner.h"
#include "h2_wolfssl.h"
#include "projects/e2e/libs/pal-net-tls-runner/runner.h"
#include <net/if.h>
int h2_ipv6_mobile_run(h2_runtime_config_t settings, const char *host,
                       uint16_t port, const char *session, const uint8_t *ca,
                       size_t ca_len, const uint8_t *wrong, size_t wrong_len,
                       const char *dns_host, const char *dns_ip,
                       const char *http_url, const char *fallback_url,
                       uint16_t mqtt_port, const char *offer_url,
                       const char *stun_url, uint16_t dns_port,
                       h2_pal_ipv6_result_t *result) {
  h2_runtime_t *runtime = NULL;
  settings.crypto = h2_wolfssl_crypto_api();
  int rc = h2_runtime_init(&settings, &runtime);
  if (rc != H2_PAL_OK)
    return rc;
  h2_net_tls_fixture_client_t client = {
      .runtime = runtime, .host = host, .session = session, .port = port};
  h2_pal_ipv6_config_t config = {
      .netif = {.type = H2_PAL_NETIF_REF_ID,
                .id = if_nametoindex(
#if defined(__APPLE__)
                    "lo0"
#else
                    "lo"
#endif
                    )},
      .local_ipv6 = {.family = H2_PAL_NET_FAMILY_IPV6,
                     .ip = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
      .dtls = h2_wolfssl_dtls_api(),
      .http_url = http_url,
      .offer_url = offer_url,
      .stun_url = stun_url,
      .fallback_url = fallback_url,
      .mqtt_host = host,
      .mqtt_port = mqtt_port,
      .transport = {.runtime = runtime,
                    .host = host,
                    .session = session,
                    .family = H2_PAL_NET_FAMILY_IPV6,
                    .root_ca = ca,
                    .root_ca_len = ca_len,
                    .wrong_ca = wrong,
                    .wrong_ca_len = wrong_len,
                    .dns_host = dns_host,
                    .server_name = "pal-net-tls.test",
                    .prepare = h2_net_tls_fixture_prepare,
                    .verify = h2_net_tls_fixture_verify,
                    .fixture_user = &client,
                    .case_timeout_ms = 90000u,
                    .report = h2_ipv6_report}};
  rc = h2_ipv6_parse_address(runtime->net, dns_ip,
                             &config.transport.dns_expected);
  if (rc == H2_PAL_OK)
    rc = h2_ipv6_parse_address(runtime->net, host, &config.dns_server);
  config.dns_answer = config.dns_server;
  config.dns_server.port = dns_port;
  if (rc == H2_PAL_OK)
    rc = h2_pal_ipv6_e2e_run(&config, result);
  h2_runtime_deinit(runtime);
  return rc;
}
