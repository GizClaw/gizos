#include "h2_desktop_platform.h"
#include "h2_pal_ipv6_runner.h"
#include "h2_webrtc_compat_factory.h"
#include "h2_wolfssl.h"
#include "projects/e2e/libs/pal-net-tls-runner/runner.h"
#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#define host_net h2_darwin_net_api
#define host_entropy h2_darwin_entropy
#else
#include "h2_linux_platform.h"
#define host_net h2_linux_net_api
#define host_entropy h2_linux_entropy
#endif
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>

static size_t read_ca(const char *path, uint8_t *bytes, size_t capacity) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return 0u;
  size_t length = fread(bytes, 1u, capacity, file);
  int valid = feof(file) && !ferror(file);
  fclose(file);
  return valid ? length : 0u;
}
int main(int argc, char **argv) {
  if (argc != 14)
    return 2;
  uint8_t ca[8192], wrong[8192];
  size_t ca_len = read_ca(argv[4], ca, sizeof(ca));
  size_t wrong_len = read_ca(argv[5], wrong, sizeof(wrong));
  if (!ca_len || !wrong_len)
    return 2;
  char *end = NULL;
  unsigned long port = strtoul(argv[2], &end, 10);
  if (!end || *end || !port || port > 65535u)
    return 2;
  h2_runtime_t runtime = {.mem = h2_desktop_platform_default_allocator(),
                          .net = host_net(),
                          .time = h2_desktop_platform_time_api()};
  h2_wolfssl_config_t tls = {.mem = *runtime.mem, .entropy = host_entropy};
  if (h2_wolfssl_init(&tls) != H2_PAL_OK)
    return 2;
  h2_webrtc_compat_backend_t backend = {0};
  if (h2_webrtc_compat_backend_create(&backend) != H2_PAL_OK) {
    (void)h2_wolfssl_deinit();
    return 2;
  }
  runtime.webrtc = backend.api;
  runtime.crypto = h2_wolfssl_crypto_api();
  h2_net_tls_fixture_client_t client = {.runtime = &runtime,
                                        .host = argv[1],
                                        .session = argv[3],
                                        .port = (uint16_t)port};
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
      .offer_url = argv[11],
      .stun_url = argv[12],
      .http_url = argv[8],
      .fallback_url = argv[9],
      .mqtt_host = argv[1],
      .mqtt_port = (uint16_t)strtoul(argv[10], NULL, 10),
      .transport = {.runtime = &runtime,
                    .host = argv[1],
                    .session = argv[3],
                    .root_ca = ca,
                    .root_ca_len = ca_len,
                    .wrong_ca = wrong,
                    .wrong_ca_len = wrong_len,
                    .dns_host = argv[6],
                    .server_name = "pal-net-tls.test",
                    .prepare = h2_net_tls_fixture_prepare,
                    .verify = h2_net_tls_fixture_verify,
                    .fixture_user = &client,
                    .report = h2_ipv6_report,
                    .multicast_supported = 1,
                    .case_timeout_ms = 30000u,
                    .family = H2_PAL_NET_FAMILY_IPV6}};
  if (h2_ipv6_parse_address(runtime.net, argv[7],
                            &config.transport.dns_expected) != H2_PAL_OK) {
    backend.destroy(backend.state);
    (void)h2_wolfssl_deinit();
    return 2;
  }
  if (h2_ipv6_parse_address(runtime.net, argv[1], &config.dns_server) !=
      H2_PAL_OK) {
    backend.destroy(backend.state);
    (void)h2_wolfssl_deinit();
    return 2;
  }
  config.dns_answer = config.dns_server;
  config.dns_server.port = (uint16_t)strtoul(argv[13], NULL, 10);
  h2_pal_ipv6_result_t result;
  int rc = h2_pal_ipv6_e2e_run(&config, &result);
  backend.destroy(backend.state);
  int teardown = h2_wolfssl_deinit();
  h2_ipv6_write_report(NULL, "macos", &result, rc, teardown);
  return rc == H2_PAL_OK && teardown == H2_PAL_OK ? 0 : 1;
}
