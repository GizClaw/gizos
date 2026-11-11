#include "h2_alipay_e2e.h"
#include "h2_alipay_e2e_target.h"
#include "h2_alipay_tiga_gateway.h"
#include "h2_e2e_fixture_wifi.h"
#include "h2_esp_alipay.h"
#include "h2_esp_board.h"
#include "h2_device_service.h"
#include "h2_esp_target_task_policy.h"
#include "h2_sha256.h"
#include "esp_app_desc.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static h2_device_app_commands_t management = {
    .config = {.hardware_capabilities = H2_LOADER_CAPABILITIES_ALL,
               .identity_policy = H2_DEVICE_IDENTITY_NATIVE,
               .digest_yield_ms = 1u,
               .command_stack_size = 49152u},
};
static h2_runtime_t* runtime;
static h2_runtime_config_t runtime_config;
static uint16_t* transferred;
static int capture_width, capture_height, capture_rc;
static size_t transfer_count;
static char run_id[33];
static bool confirmed;

static void hold(void) {
  fflush(stdout);
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
static void fail(const char* stage, int rc) {
  printf("H2_ALIPAY_E2E_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  hold();
}
static void hex(const uint8_t* data, size_t length, char* out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 15];
  }
  out[2 * length] = '\0';
}
static void capture(void* user, const h2_display_rect_t* rect,
                    const uint16_t* pixels) {
  (void)user;
  if (transferred == NULL || rect == NULL || pixels == NULL || rect->x < 0 ||
      rect->y < 0 || rect->width <= 0 || rect->height <= 0 ||
      rect->x >= capture_width || rect->y >= capture_height ||
      rect->width > capture_width - rect->x ||
      rect->height > capture_height - rect->y) {
    capture_rc = H2_PAL_ERR_IO;
    return;
  }
  const uint8_t* bytes = (const uint8_t*)pixels;
  for (int y = 0; y < rect->height; ++y)
    for (int x = 0; x < rect->width; ++x) {
      size_t offset = ((size_t)y * rect->width + x) * 2;
      transferred[(size_t)(rect->y + y) * capture_width + rect->x + x] =
          (uint16_t)((uint16_t)bytes[offset] << 8) | bytes[offset + 1];
    }
  ++transfer_count;
}
static h2_pal_result_t observe(void* user, const uint8_t* pixels, size_t stride,
                               int width, int height) {
  (void)user;
  int rc = capture_rc;
  if (width != capture_width || height != capture_height || !transfer_count)
    rc = H2_PAL_ERR_IO;
  for (int y = 0; rc == H2_PAL_OK && y < height; ++y)
    if (memcmp(pixels + (size_t)y * stride, transferred + (size_t)y * width,
               (size_t)width * 2) != 0)
      rc = H2_PAL_ERR_IO;
  printf(
      "H2_ALIPAY_E2E_DISPLAY source=completed-SPI-DMA chunks=%zu "
      "optical_verified=0 rc=%d\n",
      transfer_count, rc);
  return rc;
}
static h2_pal_result_t prepare(h2_runtime_t* rt, void* user) {
  (void)user;
  const h2_pal_wifi_sta_config_t* fixture = h2_e2e_fixture_wifi();
  int rc = fixture != NULL
               ? h2_pal_wifi_sta_connect(rt->wifi_sta, fixture, 20000)
               : h2_runtime_wifi_connect_saved(rt, 20000);
  if (rc != H2_PAL_OK) return rc;
  uint64_t started;
  rc = h2_pal_time_get_monotonic_ms(rt->time, &started);
  if (rc != H2_PAL_OK) return rc;
  for (;;) {
    h2_pal_wifi_sta_status_t status = {0};
    rc = h2_pal_wifi_sta_get_status(rt->wifi_sta, &status);
    if (rc != H2_PAL_OK) return rc;
    if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
        status.ip.ip4 != 0)
      break;
    uint64_t current;
    rc = h2_pal_time_get_monotonic_ms(rt->time, &current);
    if (rc != H2_PAL_OK) return rc;
    if (current - started >= 20000) return H2_PAL_ERR_TIMEOUT;
    rc = h2_pal_time_sleep_ms(rt->time, 100);
    if (rc != H2_PAL_OK) return rc;
  }
  puts("H2_ALIPAY_E2E_NETWORK ip=ready");
  const esp_sntp_config_t time_config =
      ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  if (esp_netif_sntp_init(&time_config) != ESP_OK) return H2_PAL_ERR_IO;
  esp_err_t time_rc = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(60000));
  esp_netif_sntp_deinit();
  printf("H2_ALIPAY_E2E_TIME server=pool.ntp.org sync_rc=%d\n", (int)time_rc);
  if (time_rc != ESP_OK) return H2_PAL_ERR_TIMEOUT;
  struct timeval wall;
  if (gettimeofday(&wall, NULL) != 0 || wall.tv_sec <= 0) return H2_PAL_ERR_IO;
  rc = h2_pal_time_set_wall_ms(
      rt->time, (uint64_t)wall.tv_sec * 1000 + (uint64_t)wall.tv_usec / 1000);
  if (rc == H2_PAL_OK) puts("H2_ALIPAY_E2E_TIME calibrated=true");
  if (rc != H2_PAL_OK) return rc;
  return h2_alipay_tiga_gateway_probe(rt);
}
static void trace_report(const h2_alipay_e2e_result_t* r) {
  const h2_alipay_api_t* api = h2_esp_alipay_api();
  const h2_alipay_trace_t* trace = api->trace;
  const char* version = esp_app_get_description()->version;
  char version_hex[2 * sizeof(esp_app_get_description()->version) + 1];
  hex((const uint8_t*)version, strlen(version), version_hex);
  printf(
      "H2_ALIPAY_META {\"schema\":1,\"run_id\":\"%s\","
      "\"case_id\":\"alipay/standalone\",\"source\":\"vendor_sdk\","
      "\"board\":\"%s\",\"target\":\"%s\",\"sdk_manifest_sha256\":\"%s\","
      "\"sdk_version\":null,\"firmware_version_hex\":\"%s\","
      "\"firmware_rc\":0,\"firmware_git_sha\":null,"
      "\"git_sha_source\":\"host_build_manifest_required\","
      "\"private_raw\":false,\"plaintext_http_available\":false,"
      "\"diagnostic_limit\":%u}\n",
      run_id, runtime->board, runtime->target, api->sdk_manifest_sha256,
      version_hex, (unsigned)trace->diagnostic_limit);
  for (size_t i = 0; i < trace->count; ++i) {
    const h2_alipay_trace_event_t* e = &trace->events[i];
    char digest[65], detail[sizeof(e->detail) * 2 + 1];
    h2_sha256_hex(e->digest, digest);
    hex((const uint8_t*)e->detail, strlen(e->detail), detail);
    printf(
        "H2_ALIPAY_EVENT {\"run_id\":\"%s\","
        "\"case_id\":\"alipay/standalone\",\"source\":\"runtime_vendor_sdk\","
        "\"seq\":%u,\"call_id\":%u,\"parent_id\":%u,\"connection_id\":%u,"
        "\"mono_ms\":%llu,\"utc_ms\":%llu,\"utc_valid\":%s,"
        "\"wall_source\":%u,\"duration_ms\":%llu,\"operation\":\"%s\","
        "\"boundary\":\"%s\",\"result\":%d,\"item\":%u,\"length\":%zu,"
        "\"captured\":%zu,\"sha256\":\"%s\",\"detail_hex\":\"%s\"}\n",
        run_id, (unsigned)e->sequence, (unsigned)e->call_id,
        (unsigned)e->parent_id, (unsigned)e->connection_id,
        (unsigned long long)e->mono_ms, (unsigned long long)e->utc_ms,
        e->utc_valid ? "true" : "false", (unsigned)e->wall_source,
        (unsigned long long)e->duration_ms, e->operation, e->boundary,
        e->result, (unsigned)e->item, e->length, e->captured, digest, detail);
  }
  printf(
      "H2_ALIPAY_RESULT {\"run_id\":\"%s\","
      "\"case_id\":\"alipay/standalone\",\"source\":\"vendor_sdk\","
      "\"stage\":%d,\"result\":%d,\"blocker\":%d,\"pre_init_rc\":%d,"
      "\"first_init\":%s,\"code_rc\":%d,\"length\":%zu,"
      "\"qr_visible\":%s,\"init_ms\":%llu,\"code_ms\":%llu}\n",
      run_id, (int)r->stage, r->result, (int)r->blocker, r->init_sdk_rc,
      r->first_init ? "true" : "false", r->code_sdk_rc, r->length,
      r->qr_visible ? "true" : "false", (unsigned long long)r->init_ms,
      (unsigned long long)r->code_ms);
  printf(
      "H2_ALIPAY_END {\"run_id\":\"%s\","
      "\"case_id\":\"alipay/standalone\",\"complete\":%s,"
      "\"dropped\":%u,\"truncated_bytes\":%zu,\"events\":%zu,"
      "\"raw_complete\":false,\"diagnostics\":{\"limit\":%u,"
      "\"vendor_logs\":{\"seen\":%u,\"summarized\":%u},"
      "\"clock_success\":{\"seen\":%u,\"summarized\":%u},"
      "\"allocation_success\":{\"seen\":%u,\"summarized\":%u}}}\n",
      run_id, trace->dropped == 0 ? "true" : "false", (unsigned)trace->dropped,
      trace->truncated_bytes, trace->count, (unsigned)trace->diagnostic_limit,
      (unsigned)trace->diagnostic_seen[0],
      (unsigned)trace->diagnostic_summarized[0],
      (unsigned)trace->diagnostic_seen[1],
      (unsigned)trace->diagnostic_summarized[1],
      (unsigned)trace->diagnostic_seen[2],
      (unsigned)trace->diagnostic_summarized[2]);
}
static void report(h2_runtime_t* rt, void* user,
                   const h2_alipay_e2e_result_t* r, bool replay) {
  (void)user;
  if (!replay) trace_report(r);
  if (!replay && r->result == H2_PAL_OK && r->qr_visible && !confirmed) {
    int rc = h2_device_app_confirm(&management, rt);
    printf("H2_ALIPAY_E2E_CONFIRM rc=%d\n", rc);
    confirmed = rc == H2_PAL_OK;
  }
  bool initialized =
      r->init_rc == H2_PAL_OK && r->init_sdk_rc == 0 && r->reused;
  bool code = r->result == H2_PAL_OK && r->qr_visible;
  bool init_blocked = r->init_rc == H2_ALIPAY_E2E_NOT_CALLED;
  bool blocked = r->code_sdk_rc == H2_ALIPAY_E2E_NOT_CALLED;
  bool display_error = r->display_rc != H2_PAL_OK;
  printf(
      "H2_ALIPAY_E2E_CASE case=alipay/pre-init source=vendor_sdk "
      "status=%s sdk_rc=%d reused=%d\n",
      initialized    ? "PASS"
      : init_blocked ? "BLOCKED"
                     : "FAIL",
      r->init_sdk_rc, r->reused);
  printf(
      "H2_ALIPAY_E2E_CASE case=alipay/binding-code source=vendor_sdk "
      "status=%s sdk_rc=%d blocker=%d qr_visible=%d display_rc=%d\n",
      display_error ? "ERROR"
      : code        ? "PASS"
      : blocked     ? "BLOCKED"
                    : "FAIL",
      r->code_sdk_rc, (int)r->blocker, r->qr_visible, r->display_rc);
  printf(
      "H2_ALIPAY_E2E_SUMMARY run_id=%s selected=2 terminal=2 pass=%u "
      "fail=%u error=%u blocked=%u setup_rc=%d display_rc=%d "
      "complete=true exit_code=%u replay=%s\n",
      run_id, (unsigned)initialized + (unsigned)code,
      (unsigned)(!initialized && !init_blocked) +
          (unsigned)(!display_error && !code && !blocked),
      (unsigned)display_error,
      (unsigned)init_blocked + (unsigned)(!display_error && !code && blocked),
      r->prepare_rc, r->display_rc, (unsigned)(!initialized || !code),
      replay ? "true" : "false");
  fflush(stdout);
}
static void run(void* user) {
  (void)user;
  int rc = h2_alipay_e2e_target_configure(runtime);
  if (rc != H2_PAL_OK) fail("alipay-configure", rc);
  const h2_alipay_api_t* api = h2_esp_alipay_api();
  if (api == NULL || api->source != H2_ALIPAY_SOURCE_VENDOR_SDK ||
      api->trace == NULL)
    fail("vendor-sdk", H2_PAL_ERR_INVALID_STATE);
  uint8_t nonce[16];
  rc = h2_pal_crypto_random(runtime->crypto, nonce, sizeof(nonce));
  if (rc != H2_PAL_OK) fail("run-id", rc);
  hex(nonce, sizeof(nonce), run_id);
  h2_display_info_t info = h2_alipay_e2e_target_capture(NULL, NULL);
  if (info.width < 240 || info.height < 240 || info.width > 1024 ||
      info.height > 1024 || info.native_format != H2_DISPLAY_PIXEL_RGB565)
    fail("capture-geometry", H2_PAL_ERR_UNSUPPORTED);
  capture_width = info.width;
  capture_height = info.height;
  size_t bytes = (size_t)info.width * info.height * 2;
  transferred = h2_pal_mem_alloc(runtime->mem, bytes);
  if (transferred == NULL) fail("capture-memory", H2_PAL_ERR_NO_MEMORY);
  memset(transferred, 0, bytes);
  (void)h2_alipay_e2e_target_capture(capture, NULL);
  const h2_alipay_e2e_config_t config = {
      .alipay = api,
      .feature = h2_alipay_e2e_target_feature(),
      .prepare = prepare,
      .observe = observe,
      .report = report,
  };
  rc = h2_alipay_e2e_run(runtime, &config);
  (void)h2_alipay_e2e_target_capture(NULL, NULL);
  h2_pal_mem_free(runtime->mem, transferred);
  transferred = NULL;
  fail("app-returned", rc);
}
static void image_entry(void* user) {
  (void)user;
  int rc = h2_esp_board_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) fail("board", rc);
  rc = h2_device_app_commands_prepare_named(&management, &runtime_config,
                                            "alipay-e2e", 1u, 3u);
  if (rc != H2_PAL_OK) fail("commands", rc);
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) fail("runtime", rc);
  printf("H2_ALIPAY_E2E_BOOT board=%s target=%s version=%s\n", runtime->board,
         runtime->target, esp_app_get_description()->version);
  run(NULL);
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install_with_configure(
      h2_esp_board_task_configure);
  if (rc == H2_PAL_OK)
    rc = h2_esp_board_start_entry_task("alipay/e2e/runner", image_entry, NULL);
  if (rc != H2_PAL_OK) fail("entry", rc);
}
