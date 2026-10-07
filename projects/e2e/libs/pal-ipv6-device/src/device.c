#include "h2_pal_ipv6_device.h"
#include "h2_pal_ipv6_fixture_config.h"
#include "h2_pal_ipv6_runner.h"
#include "projects/e2e/libs/pal-net-tls-runner/runner.h"
#include <stdio.h>
#include <string.h>
const char h2_pal_ipv6_device_task_name[] = "pal-ipv6/e2e/runner";
int h2_pal_ipv6_device_board_fixture(void) {
  return H2_PAL_IPV6_BOARD_FIXTURE != 0;
}
const char *h2_pal_ipv6_device_host(void) { return H2_PAL_IPV6_HOST; }
static char boot_id[17];
static int hex(char value) {
  return value >= '0' && value <= '9'   ? value - '0'
         : value >= 'a' && value <= 'f' ? value - 'a' + 10
                                        : -1;
}
static uint8_t *decode(const h2_pal_mem_api_t *mem, const char *text,
                       size_t *length) {
  size_t size = strlen(text);
  if (!size || size % 2u || size > 16384u)
    return NULL;
  uint8_t *out = h2_pal_mem_alloc(mem, size / 2u);
  if (!out)
    return NULL;
  for (size_t i = 0u; i < size; i += 2u) {
    int high = hex(text[i]), low = hex(text[i + 1u]);
    if (high < 0 || low < 0) {
      h2_pal_mem_free(mem, out);
      return NULL;
    }
    out[i / 2u] = (uint8_t)((high << 4) | low);
  }
  *length = size / 2u;
  return out;
}
void h2_pal_ipv6_device_report(const h2_runtime_t *runtime,
                               const h2_pal_ipv6_result_t *result) {
  char line[512];
  for (unsigned i = 0u; i < H2_PAL_IPV6_CASES; ++i) {
    const h2_net_tls_case_result_t *item = &result->cases[i];
    if (!item->id)
      continue;
    snprintf(line, sizeof(line),
             "H2_PAL_IPV6_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,"
             "\"line\":%u,\"tx\":%zu,\"rx\":%zu,\"boot_id\":\"%s\"}",
             item->id,
             item->passed    ? "PASS"
             : item->blocked ? "BLOCKED"
                             : "FAIL",
             item->detail, item->line, item->bytes_sent, item->bytes_received,
             boot_id);
    h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-ipv6", line);
    h2_pal_time_sleep_ms(runtime->time, 40u);
  }
  snprintf(line, sizeof(line),
           "H2_PAL_IPV6_SUMMARY {\"passed\":%u,\"failed\":%u,\"blocked\":%u,"
           "\"retained_sockets\":%zu,\"retained_resolvers\":%zu,\"retained_"
           "allocations\":%zu,\"retained_tasks\":%zu,\"cleanup_error\":%d,"
           "\"boot_id\":\"%s\"}",
           result->passed, result->failed, result->blocked,
           result->retained_sockets, result->retained_resolvers,
           result->retained_allocations, result->retained_tasks,
           result->cleanup_error, boot_id);
  h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-ipv6", line);
}
int h2_pal_ipv6_device_run(h2_runtime_t *runtime, const h2_pal_dtls_api_t *dtls,
                           h2_pal_ipv6_result_t *result) {
  if (!runtime || !result)
    return H2_PAL_ERR_INVALID_ARG;
  if (result->retained_cleanup || result->retained_runtime)
    return H2_PAL_ERR_INVALID_STATE;
  memset(result, 0, sizeof(*result));
  if (!H2_PAL_IPV6_HOST[0] || strlen(H2_PAL_IPV6_SESSION) != 32u ||
      !H2_PAL_IPV6_PORT || H2_PAL_IPV6_PORT > UINT16_MAX ||
      !H2_PAL_IPV6_EPOCH_MS)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_net_addr_t local;
  int rc = h2_pal_net_get_host_addr_family(runtime->net, NULL,
                                           H2_PAL_NET_FAMILY_IPV6, &local);
  if (rc != H2_PAL_OK)
    return H2_PAL_ERR_UNAVAILABLE;
  rc = h2_pal_time_set_wall_ms(runtime->time, H2_PAL_IPV6_EPOCH_MS);
  if (rc != H2_PAL_OK)
    return rc;
  size_t ca_len = 0u, wrong_len = 0u;
  uint8_t *ca = decode(runtime->mem, H2_PAL_IPV6_CA_HEX, &ca_len);
  uint8_t *wrong = decode(runtime->mem, H2_PAL_IPV6_WRONG_CA_HEX, &wrong_len);
  if (!ca || !wrong) {
    h2_pal_mem_free(runtime->mem, ca);
    h2_pal_mem_free(runtime->mem, wrong);
    return H2_PAL_ERR_NO_MEMORY;
  }
  uint8_t nonce[8];
  rc = h2_pal_crypto_random(runtime->crypto, nonce, sizeof(nonce));
  if (rc != H2_PAL_OK)
    goto cleanup;
  const char digits[] = "0123456789abcdef";
  for (size_t i = 0u; i < sizeof(nonce); ++i) {
    boot_id[i * 2u] = digits[nonce[i] >> 4];
    boot_id[i * 2u + 1u] = digits[nonce[i] & 15u];
  }
  char execution[160];
  snprintf(execution, sizeof(execution),
           "H2_PAL_IPV6_EXECUTION session=%s boot_id=%s", H2_PAL_IPV6_SESSION,
           boot_id);
  h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-ipv6", execution);
  h2_net_tls_fixture_client_t client = {.runtime = runtime,
                                        .host = H2_PAL_IPV6_HOST,
                                        .port = (uint16_t)H2_PAL_IPV6_PORT,
                                        .session = H2_PAL_IPV6_SESSION,
                                        .run_id = boot_id};
  h2_pal_ipv6_config_t config = {
      .dtls = dtls,
      .local_ipv6 = local,
      .netif = {.type = H2_PAL_NETIF_REF_DEFAULT},
      .http_url = H2_PAL_IPV6_HTTP_URL,
      .fallback_url = H2_PAL_IPV6_FALLBACK_URL,
      .offer_url = H2_PAL_IPV6_OFFER_URL,
      .stun_url = H2_PAL_IPV6_STUN_URL,
      .mqtt_host = H2_PAL_IPV6_HOST,
      .mqtt_port = (uint16_t)H2_PAL_IPV6_MQTT_PORT,
      .transport = {.runtime = runtime,
                    .family = H2_PAL_NET_FAMILY_IPV6,
                    .host = H2_PAL_IPV6_HOST,
                    .session = H2_PAL_IPV6_SESSION,
                    .dns_host = H2_PAL_IPV6_DNS_HOST,
                    .root_ca = ca,
                    .root_ca_len = ca_len,
                    .wrong_ca = wrong,
                    .wrong_ca_len = wrong_len,
                    .server_name = "pal-net-tls.test",
                    .prepare = h2_net_tls_fixture_prepare,
                    .verify = h2_net_tls_fixture_verify,
                    .fixture_user = &client,
                    .case_timeout_ms = 30000u}};
  rc = h2_ipv6_parse_address(runtime->net, H2_PAL_IPV6_DNS_IPV6,
                             &config.transport.dns_expected);
  if (rc == H2_PAL_OK)
    rc = h2_ipv6_parse_address(runtime->net, H2_PAL_IPV6_HOST,
                               &config.dns_server);
  config.dns_answer = config.dns_server;
  config.dns_server.port = (uint16_t)H2_PAL_IPV6_DNS_PORT;
  if (rc == H2_PAL_OK)
    rc = h2_pal_ipv6_e2e_run(&config, result);
cleanup:
  h2_pal_mem_free(runtime->mem, ca);
  h2_pal_mem_free(runtime->mem, wrong);
  return rc;
}
