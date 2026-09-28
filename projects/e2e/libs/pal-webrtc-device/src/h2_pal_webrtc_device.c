#include "h2_pal_webrtc_device.h"
#include "client.h"
#include "h2_corehttp.h"
#include "h2_pal_webrtc_fixture_config.h"
#include <stdio.h>
#include <string.h>

const char h2_pal_webrtc_device_runner_task_name[] = "pal-webrtc/e2e/runner";
static char fixture_run[17];

static void report(void *user, const h2_pal_webrtc_e2e_case_result_t *result) {
  const h2_runtime_t *runtime = user;
  char line[256];
  (void)snprintf(line, sizeof(line),
                 "H2_PAL_WEBRTC_CASE "
                 "{\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,"
                 "\"elapsed_ms\":%llu}\n",
                 result->id,
                 result->passed    ? "PASS"
                 : result->blocked ? "BLOCKED"
                                   : "FAIL",
                 result->detail, result->line,
                 (unsigned long long)result->elapsed_ms);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-webrtc", line);
  if (runtime != NULL)
    (void)h2_pal_time_sleep_ms(runtime->time, 40u);
}

void h2_pal_webrtc_device_report(const h2_runtime_t *runtime,
                                 const h2_pal_webrtc_e2e_result_t *result) {
  h2_pal_firmware_info_t image = {0};
  if (h2_pal_firmware_info_get_current(runtime->firmware_info, &image) !=
      H2_PAL_OK)
    return;
  char run_line[256];
  (void)snprintf(run_line, sizeof(run_line),
                 "H2_PAL_WEBRTC_RUN id=%s version=%s", fixture_run,
                 image.version);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-webrtc", run_line);
  for (unsigned index = 0u; index < H2_PAL_WEBRTC_E2E_CASE_COUNT; ++index)
    if (result->cases[index].id != NULL)
      report((void *)runtime, &result->cases[index]);
  char line[256];
  (void)snprintf(line, sizeof(line),
                 "H2_PAL_WEBRTC_SUMMARY "
                 "{\"passed\":%u,\"failed\":%u,\"blocked\":%u,\"retained_"
                 "allocations\":%zu,\"cleanup\":0}\n",
                 result->passed, result->failed, result->blocked,
                 result->retained_allocations);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-webrtc", line);
}

static int connect_saved_wifi(h2_runtime_t *runtime) {
  h2_pal_wifi_sta_config_t wifi = {0};
  int rc =
      h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &wifi);
  if (rc == H2_PAL_OK)
    rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &wifi, 20000u);
  memset(&wifi, 0, sizeof(wifi));
  if (rc != H2_PAL_OK)
    return rc;
  uint64_t start = 0u;
  rc = h2_pal_time_get_monotonic_ms(runtime->time, &start);
  while (rc == H2_PAL_OK) {
    h2_pal_wifi_sta_status_t status = {0};
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (rc != H2_PAL_OK)
      return rc;
    if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid)
      return H2_PAL_OK;
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (now - start >= 20000u)
      return H2_PAL_ERR_TIMEOUT;
    if (rc == H2_PAL_OK)
      rc = h2_pal_time_sleep_ms(runtime->time, 100u);
  }
  return rc;
}

int h2_pal_webrtc_device_run(h2_runtime_t *runtime,
                             uint32_t connection_timeout_ms,
                             h2_pal_webrtc_e2e_result_t *result) {
  if (runtime == NULL || result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(result, 0, sizeof(*result));
  if (!H2_PAL_WEBRTC_OFFER_URL[0] || !H2_PAL_WEBRTC_STUN_URL[0])
    return H2_PAL_ERR_INVALID_ARG;
  int rc = connect_saved_wifi(runtime);
  if (rc != H2_PAL_OK)
    return rc;
  h2_corehttp_t *provider = NULL;
  h2_pal_http_api_t http = {0};
  const h2_corehttp_config_t provider_config = {
      .allocator = runtime->mem,
      .net = runtime->net,
      .time = runtime->time,
      .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
      .io_slice_ms = 20u,
  };
  rc = h2_corehttp_create(&provider_config, &provider, &http);
  if (rc != H2_PAL_OK)
    return rc;
  uint8_t nonce[8];
  rc = h2_pal_crypto_random(runtime->crypto, nonce, sizeof(nonce));
  if (rc != H2_PAL_OK) {
    h2_corehttp_destroy(provider);
    return rc;
  }
  const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(nonce); ++i) {
    fixture_run[i * 2u] = digits[nonce[i] >> 4];
    fixture_run[i * 2u + 1u] = digits[nonce[i] & 15u];
  }
  h2_webrtc_fixture_client_t client = {.http = &http,
                                       .offer_url = H2_PAL_WEBRTC_OFFER_URL};
  const h2_pal_webrtc_e2e_config_t config = {
      .runtime = runtime,
      .stun_url = H2_PAL_WEBRTC_STUN_URL,
      .connection_timeout_ms = connection_timeout_ms,
      .exchange_offer = h2_webrtc_fixture_exchange,
      .close_remote = h2_webrtc_fixture_close,
      .fixture_user = &client,
      .report = report,
      .report_user = runtime,
  };
  rc = h2_pal_webrtc_e2e_run(&config, result);
  h2_corehttp_destroy(provider);
  return rc;
}
