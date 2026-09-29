#include "h2_pal_net_tls_device.h"
#include "h2_pal_net_tls_fixture_config.h"
#include <stdio.h>
#include <string.h>
const char h2_pal_net_tls_device_runner_task_name[] = "pal-net-tls/e2e/runner";
static char boot_id[17];
static int hex_digit(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
static uint8_t *decode(const h2_pal_mem_api_t *mem, const char *hex,
                       size_t *length) {
  size_t size = strlen(hex);
  if (!size || size % 2u || size > 16384u)
    return NULL;
  uint8_t *pem = h2_pal_mem_alloc(mem, size / 2u);
  if (!pem)
    return NULL;
  for (size_t i = 0u; i < size; i += 2u) {
    int high = hex_digit(hex[i]), low = hex_digit(hex[i + 1u]);
    if (high < 0 || low < 0) {
      h2_pal_mem_free(mem, pem);
      return NULL;
    }
    pem[i / 2u] = (uint8_t)((high << 4) | low);
  }
  *length = size / 2u;
  return pem;
}
static void report(void *user, const h2_net_tls_case_result_t *item) {
  const h2_runtime_t *runtime = user;
  char line[512];
  snprintf(
      line, sizeof(line),
      "H2_PAL_NET_TLS_CASE "
      "{\"id\":\"%s\",\"mandatory\":%s,\"status\":\"%s\",\"detail\":%d,"
      "\"line\":%u,\"elapsed_ms\":%llu,\"bytes_sent\":%zu,\"bytes_"
      "received\":%zu,\"provider_result\":%d,\"observed_ipv4\":[%u,%u,%u,%u]}",
      item->id, item->mandatory ? "true" : "false",
      item->passed         ? "PASS"
      : item->blocked      ? "BLOCKED"
      : item->unsupported  ? "UNSUPPORTED"
      : item->not_assessed ? "NOT_ASSESSED"
                           : "FAIL",
      item->detail, item->line, (unsigned long long)item->elapsed_ms,
      item->bytes_sent, item->bytes_received, item->provider_result,
      (unsigned)item->observed_ipv4[0], (unsigned)item->observed_ipv4[1],
      (unsigned)item->observed_ipv4[2], (unsigned)item->observed_ipv4[3]);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-net-tls", line);
  (void)h2_pal_time_sleep_ms(runtime->time, 40u);
}
void h2_pal_net_tls_device_report(const h2_runtime_t *runtime,
                                  const h2_net_tls_result_t *result) {
  h2_pal_firmware_info_t image = {0};
  int version =
      h2_pal_firmware_info_get_current(runtime->firmware_info, &image);
  char line[512];
  snprintf(line, sizeof(line),
           "H2_PAL_NET_TLS_RUN session=%s boot_id=%s version=%s version_rc=%d "
           "dns_host=%s dns_ipv4=%s",
           H2_PAL_NET_TLS_SESSION, boot_id,
           version == H2_PAL_OK ? image.version : "unavailable", version,
           H2_PAL_NET_TLS_DNS_HOST, H2_PAL_NET_TLS_DNS_IPV4);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-net-tls", line);
  for (unsigned i = 0u; i < H2_NET_TLS_CASE_COUNT; ++i)
    if (result->cases[i].id)
      report((void *)runtime, &result->cases[i]);
  snprintf(line, sizeof(line),
           "H2_PAL_NET_TLS_SUMMARY "
           "{\"profile\":\"net-tls-core\",\"full_net_qualified\":false,"
           "\"passed\":%u,\"mandatory_passed\":%u,\"failed\":%u,\"blocked\":%u,"
           "\"unsupported\":%u,\"not_assessed\":%u,\"retained_sockets\":%zu,"
           "\"retained_resolvers\":%zu,\"retained_allocations\":%zu}",
           result->passed, result->mandatory_passed, result->failed,
           result->blocked, result->unsupported, result->not_assessed,
           result->retained_sockets, result->retained_resolvers,
           result->retained_allocations);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-net-tls", line);
}
int h2_pal_net_tls_device_run(h2_runtime_t *runtime,
                              h2_net_tls_result_t *result) {
  if (!runtime || !result)
    return H2_PAL_ERR_INVALID_ARG;
  memset(result, 0, sizeof(*result));
  if (!H2_PAL_NET_TLS_HOST[0] || strlen(H2_PAL_NET_TLS_SESSION) != 32u ||
      !H2_PAL_NET_TLS_PORT || H2_PAL_NET_TLS_PORT > 65535u ||
      !H2_PAL_NET_TLS_EPOCH_MS)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_net_tls_device_prepare_network(runtime);
  if (rc != H2_PAL_OK)
    return rc;
  rc = h2_pal_time_set_wall_ms(runtime->time, H2_PAL_NET_TLS_EPOCH_MS);
  if (rc != H2_PAL_OK)
    return rc;
  size_t ca_len = 0u, wrong_len = 0u;
  uint8_t *ca = decode(runtime->mem, H2_PAL_NET_TLS_CA_HEX, &ca_len);
  uint8_t *wrong =
      decode(runtime->mem, H2_PAL_NET_TLS_WRONG_CA_HEX, &wrong_len);
  if (!ca || !wrong) {
    h2_pal_mem_free(runtime->mem, ca);
    h2_pal_mem_free(runtime->mem, wrong);
    return H2_PAL_ERR_NO_MEMORY;
  }
  uint8_t nonce[8];
  rc = h2_pal_crypto_random(runtime->crypto, nonce, sizeof(nonce));
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(runtime->mem, wrong);
    h2_pal_mem_free(runtime->mem, ca);
    return rc;
  }
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0u; i < sizeof(nonce); ++i) {
    boot_id[i * 2u] = digits[nonce[i] >> 4];
    boot_id[i * 2u + 1u] = digits[nonce[i] & 15u];
  }
  h2_net_tls_fixture_client_t client = {.runtime = runtime,
                                        .host = H2_PAL_NET_TLS_HOST,
                                        .session = H2_PAL_NET_TLS_SESSION,
                                        .port = (uint16_t)H2_PAL_NET_TLS_PORT,
                                        .run_id = boot_id};
  char execution[128];
  snprintf(execution, sizeof(execution),
           "H2_PAL_NET_TLS_EXECUTION session=%s boot_id=%s",
           H2_PAL_NET_TLS_SESSION, boot_id);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-net-tls",
                         execution);
  h2_net_tls_config_t config = {
      .runtime = runtime,
      .host = H2_PAL_NET_TLS_HOST,
      .session = H2_PAL_NET_TLS_SESSION,
      .root_ca = ca,
      .root_ca_len = ca_len,
      .wrong_ca = wrong,
      .wrong_ca_len = wrong_len,
      .dns_host = H2_PAL_NET_TLS_DNS_HOST,
      .server_name = "pal-net-tls.test",
      .prepare = h2_net_tls_fixture_prepare,
      .verify = h2_net_tls_fixture_verify,
      .fixture_user = &client,
      .report = report,
      .report_user = runtime,
      .case_timeout_ms = 60000u,
      .multicast_supported = runtime->net && runtime->net->vtable &&
                             runtime->net->vtable->udp_join_multicast != NULL};
  rc = h2_net_tls_parse_ipv4(H2_PAL_NET_TLS_DNS_IPV4, &config.dns_expected);
  if (rc == H2_PAL_OK)
    rc = h2_pal_net_tls_e2e_run(&config, result);
  h2_pal_mem_free(runtime->mem, wrong);
  h2_pal_mem_free(runtime->mem, ca);
  return rc;
}
