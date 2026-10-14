#include "h2_desktop_application_host.h"
#include "h2_desktop_platform.h"
#include "runner.h"
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
static h2_net_tls_config_t config;
static h2_net_tls_fixture_client_t client;
static h2_net_tls_result_t result;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)host; (void)user;
  config.runtime = runtime;
  client.runtime = runtime;
  return h2_pal_net_tls_e2e_run(&config, &result);
}
int main(int argc, char **argv) {
  if (argc != 8)
    return 2;
  static uint8_t ca[8192], wrong[8192];
  size_t ca_len = read_ca(argv[4], ca, sizeof(ca));
  size_t wrong_len = read_ca(argv[5], wrong, sizeof(wrong));
  if (!ca_len || !wrong_len)
    return 2;
  char *end = NULL;
  unsigned long port = strtoul(argv[2], &end, 10);
  if (!end || *end || !port || port > 65535u)
    return 2;
  client = (h2_net_tls_fixture_client_t){
                                        .host = argv[1],
                                        .session = argv[3],
                                        .port = (uint16_t)port};
  config = (h2_net_tls_config_t){
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
                                .report = h2_net_tls_report,
                                .multicast_supported = 1,
                                .case_timeout_ms = 30000u};
  if (h2_net_tls_parse_ipv4(argv[7], &config.dns_expected) != H2_PAL_OK) {
    return 2;
  }
  const h2_desktop_application_host_config_t platform = {.services = H2_DESKTOP_HOST_CRYPTO};
  const h2_application_host_config_t app = {.entry = run};
  h2_application_host_result_t lifecycle = {0};
  int rc = h2_desktop_application_host_run(&platform, &app, &lifecycle);
  int teardown = lifecycle.cleanup;
  h2_net_tls_summary(stdout, &result, rc, teardown);
  return rc == H2_PAL_OK && teardown == H2_PAL_OK && !lifecycle.retained ? 0 : 1;
}
