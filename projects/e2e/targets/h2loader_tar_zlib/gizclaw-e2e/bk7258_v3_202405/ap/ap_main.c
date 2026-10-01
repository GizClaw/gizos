#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_target_task_policy.h"
#include "h2_gizclaw_e2e.h"
#include "h2_gizclaw_e2e_bk_config.h"
#include "h2_gizclaw_e2e_task_names.h"
#include "h2_atomic.h"
#include "h2_corehttp.h"
#include "ledger.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_firmware_info.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include <os/os.h>
#include <stdio.h>
#include <string.h>

extern const unsigned char h2_gizclaw_e2e_pcm[];
extern const size_t h2_gizclaw_e2e_pcm_size;
static h2_runtime_t *runtime;
static h2_gizclaw_e2e_result_t result;
static h2_gizclaw_e2e_config_t app_config;
#define H2_GIZCLAW_E2E_LEDGER_CAPACITY (384u * 1024u)
static h2_gizclaw_e2e_ledger_t ledger;
static h2_pal_mutex_t *evidence_mutex;
static h2_atomic_bool_t capture_failed;
static char execution[33];
static h2_pal_firmware_info_t firmware_info;
static h2_corehttp_t *fixture_http;
static h2_pal_http_api_t fixture_http_api;
static bool admitted;
static int confirm_rc = H2_PAL_ERR_INVALID_STATE;

static void capture_evidence(void *user, const char *record, size_t size) {
  (void)user;
  if (h2_pal_mutex_lock(runtime->sync, evidence_mutex) != H2_PAL_OK) {
    h2_atomic_store_explicit(&capture_failed, true, H2_ATOMIC_RELEASE);
    return;
  }
  h2_gizclaw_e2e_ledger_append(&ledger, record, size);
  if (ledger.error != H2_PAL_OK)
    h2_atomic_store_explicit(&capture_failed, true, H2_ATOMIC_RELEASE);
  if (h2_pal_mutex_unlock(runtime->sync, evidence_mutex) != H2_PAL_OK)
    h2_atomic_store_explicit(&capture_failed, true, H2_ATOMIC_RELEASE);
}

static void replay_ledger(void) {
  if (!ledger.frozen)
    return;
  printf("H2_GIZCLAW_LEDGER stage=begin version=%s execution=%s bytes=%zu "
         "records=%zu crc32=%08x admitted=%d confirm_rc=%d physical_audio=%d\n",
         firmware_info.version, execution, ledger.size, ledger.records,
         (unsigned)ledger.crc32, admitted ? 1 : 0, confirm_rc,
         h2_gizclaw_e2e_fixture_physical_audio());
  /* SDK printf owns the board console. Its newlib FILE stdout is a separate
   * stream, so fwrite/fputs cannot deliver this UART evidence. Each captured
   * record is independently bounded by the portable formatter. */
  for (size_t offset = 0u; offset < ledger.size;) {
    const char *end = memchr(ledger.data + offset, '\n', ledger.size - offset);
    if (end == NULL)
      break;
    size_t size = (size_t)(end - (ledger.data + offset)) + 1u;
    printf("%.*s", (int)size, ledger.data + offset);
    offset += size;
  }
  printf("H2_GIZCLAW_LEDGER stage=end execution=%s crc32=%08x\n", execution,
         (unsigned)ledger.crc32);
  fflush(stdout);
}
static void hold(void) {
  for (;;)
    rtos_delay_milliseconds(1000u);
}
static void fail(const char *stage, int rc) {
  printf("H2_GIZCLAW_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
  fflush(stdout);
  hold();
}
static int network_ready(void) {
  h2_pal_wifi_sta_status_t status = {0};
  int rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
  if (!rc && status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
      status.ip.ip4)
    return H2_PAL_OK;
  h2_pal_wifi_sta_config_t saved = {0};
  rc =
      h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &saved);
  if (!rc)
    rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, &saved, 20000u);
  memset(&saved, 0, sizeof(saved));
  if (rc)
    return rc;
  for (unsigned attempt = 0u; attempt < 200u; ++attempt) {
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &status);
    if (rc)
      return rc;
    if (status.state == H2_PAL_WIFI_STA_STATE_GOT_IP && status.ip_valid &&
        status.ip.ip4)
      return H2_PAL_OK;
    rtos_delay_milliseconds(100u);
  }
  return H2_PAL_ERR_TIMEOUT;
}

static int create_fixture_http(h2_runtime_config_t *config) {
  const char *hex = h2_gizclaw_e2e_fixture_root_ca_hex();
  size_t size = strlen(hex);
  if (!size || size % 2u || size > 32768u)
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t *pem = h2_pal_mem_alloc(config->mem, size / 2u);
  if (!pem)
    return H2_PAL_ERR_NO_MEMORY;
  for (size_t i = 0u; i < size; i += 2u) {
    unsigned high = hex[i] <= '9' ? (unsigned)(hex[i] - '0')
                                  : (unsigned)(hex[i] - 'a') + 10u;
    unsigned low = hex[i + 1u] <= '9' ? (unsigned)(hex[i + 1u] - '0')
                                      : (unsigned)(hex[i + 1u] - 'a') + 10u;
    if (high > 15u || low > 15u) {
      h2_pal_mem_free(config->mem, pem);
      return H2_PAL_ERR_INVALID_ARG;
    }
    pem[i / 2u] = (uint8_t)((high << 4u) | low);
  }
  const h2_corehttp_config_t http = {
      .allocator = config->mem, .net = config->net, .time = config->time,
      .log = config->log, .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
      .root_ca_pem = pem, .root_ca_pem_len = size / 2u};
  int rc = h2_corehttp_create(&http, &fixture_http, &fixture_http_api);
  h2_pal_mem_free(config->mem, pem);
  if (rc == H2_PAL_OK)
    config->http = &fixture_http_api;
  return rc;
}
static void run(void *user) {
  (void)user;
  int rc;
  if ((!h2_gizclaw_e2e_fixture_key()[0] ||
       !h2_gizclaw_e2e_fixture_profile()[0] || !h2_gizclaw_e2e_fixture_value()[0] ||
       !h2_gizclaw_e2e_fixture_endpoint()[0] || !h2_gizclaw_e2e_fixture_token()[0] ||
       !h2_gizclaw_e2e_fixture_device_api_url()[0] || !h2_gizclaw_e2e_fixture_audio_url()[0])) {
    printf("H2_GIZCLAW_SETUP_FAIL stage=missing_app_config_fixture rc=%d\n",
           H2_PAL_ERR_INVALID_ARG);
    fflush(stdout);
    return;
  }
  while ((rc = network_ready()) != H2_PAL_OK) {
    printf("H2_GIZCLAW_SETUP_WAIT stage=network rc=%d\n", rc);
    fflush(stdout);
    rtos_delay_milliseconds(3000u);
  }
  char *evidence = h2_pal_mem_alloc(runtime->mem, H2_GIZCLAW_E2E_LEDGER_CAPACITY);
  const h2_pal_mutex_config_t mutex_config = {
      .name = "gizclaw-e2e-evidence", .allocator = runtime->mem};
  uint8_t nonce[16];
  if (evidence == NULL)
    fail("evidence_buffer", H2_PAL_ERR_NO_MEMORY);
  rc = h2_gizclaw_e2e_ledger_init(&ledger, evidence,
                                 H2_GIZCLAW_E2E_LEDGER_CAPACITY);
  if (rc)
    fail("evidence_ledger", rc);
  if (h2_atomic_init(&capture_failed, false) != H2_ATOMIC_OK)
    fail("evidence_atomic", H2_PAL_ERR_NO_MEMORY);
  rc = h2_pal_mutex_create(runtime->sync, &mutex_config, &evidence_mutex);
  if (rc)
    fail("evidence_mutex", rc);
  rc = h2_pal_crypto_random(runtime->crypto, nonce, sizeof(nonce));
  if (rc)
    fail("evidence_random", rc);
  rc = h2_pal_firmware_info_get_current(runtime->firmware_info, &firmware_info);
  if (rc)
    fail("evidence_firmware", rc);
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0u; i < sizeof(nonce); ++i) {
    execution[i * 2u] = hex[nonce[i] >> 4u];
    execution[i * 2u + 1u] = hex[nonce[i] & 15u];
  }
  execution[32] = '\0';
  printf("H2_GIZCLAW_BOOT board=bk7258 version=%s execution=%s\n",
         firmware_info.version, execution);
  fflush(stdout);
  const h2_gizclaw_e2e_bk_config_t *settings = h2_gizclaw_e2e_bk_config();
  app_config = (h2_gizclaw_e2e_config_t){
      .server_endpoint = settings->server_endpoint,
      .registration_token = settings->registration_token,
      .app_config_key = h2_gizclaw_e2e_fixture_key(),
      .expected_runtime_profile = h2_gizclaw_e2e_fixture_profile(),
      .app_config_expected_value = {h2_gizclaw_e2e_fixture_value(),
                                   strlen(h2_gizclaw_e2e_fixture_value())},
      .voice_audio = h2_gizclaw_e2e_fixture_physical_audio() ? runtime->audio : NULL,
      .voice_pcm_s16le_16khz_mono = h2_gizclaw_e2e_pcm,
      .voice_pcm_len = h2_gizclaw_e2e_pcm_size,
      .device_api_url = h2_gizclaw_e2e_fixture_device_api_url(),
      .device_audio_url = h2_gizclaw_e2e_fixture_audio_url(),
      .device_real_audio = h2_gizclaw_e2e_fixture_physical_audio() != 0,
      .on_evidence = capture_evidence,
      .suites = H2_GIZCLAW_E2E_SUITE_ALL};
  rc = h2_gizclaw_e2e_run(runtime, &app_config, &result);
  char summary[1024];
  int size = snprintf(summary, sizeof(summary),
         "H2_GIZCLAW_E2E stage=summary platform=bk7258 endpoint=%.*s "
         "backend=h2peer "
         "suite=all profile=%s selected=%zu terminal=%zu pass=%zu fail=%zu "
         "error=%zu "
         "blocked=%zu cancelled=%zu first_failure_case=%s first_failure_rc=%d "
         "cleanup_rc=%d retained_resources=%zu complete=%s exit_code=%d\n",
         (int)settings->server_endpoint.len, settings->server_endpoint.data,
         result.runtime_profile_name[0] ? result.runtime_profile_name : "-",
         result.selected, result.terminal, result.passed, result.failed,
         result.errors, result.blocked, result.cancelled,
         result.first_failure_case[0] ? result.first_failure_case : "-",
         result.first_failure_rc, result.cleanup_rc, result.retained_resources,
         result.complete ? "true" : "false", rc);
  /* The portable runner has joined all owners before a zero-retention return.
   * A retained callback keeps the capture buffer alive and prohibits freezing
   * or confirmation, even if the transport stops producing output. */
  if (result.retained_resources == 0u &&
      !h2_atomic_load_explicit(&capture_failed, H2_ATOMIC_ACQUIRE)) {
    if (size > 0 && (size_t)size < sizeof(summary))
      capture_evidence(NULL, summary, (size_t)size);
    else
      h2_atomic_store_explicit(&capture_failed, true, H2_ATOMIC_RELEASE);
    if (!h2_atomic_load_explicit(&capture_failed, H2_ATOMIC_ACQUIRE) &&
        h2_gizclaw_e2e_ledger_freeze(&ledger) == H2_PAL_OK && rc == 0 &&
        h2_gizclaw_e2e_result_all_passed(&result)) {
      confirm_rc = h2_bk_h2loader_confirm_current_app(runtime);
      admitted = confirm_rc == H2_PAL_OK;
    }
  }
  if (size > 0 && (size_t)size < sizeof(summary))
    printf("%.*s", size, summary);
  printf("H2_GIZCLAW_READY board=bk7258 rc=%d confirm=%d admitted=%d\n", rc,
         confirm_rc, admitted ? 1 : 0);
  fflush(stdout);
  for (;;) {
    replay_ledger();
    rtos_delay_milliseconds(10000u);
  }
}
static void entry(void *user) {
  (void)user;
  h2_runtime_config_t config = {0};
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc)
    fail("board", rc);
  /* Large business fixtures and the immutable ledger belong to this test
   * Runtime's PSRAM allocator, leaving SDK networking SRAM available. */
  config.mem = h2_bk7258_board_psram_allocator();
  /* BK has no system CA bundle. This launcher owns an explicit verified HTTP
   * provider for its full recovery lifetime; the BSP provider stays owned by
   * the BSP. No production trust policy or VERIFY_NONE fallback is changed. */
  rc = create_fixture_http(&config);
  if (rc) {
    h2_bk7258_board_runtime_deinit();
    fail("http_trust", rc);
  }
  rc = h2_runtime_init(&config, &runtime);
  if (rc) {
    h2_corehttp_destroy(fixture_http);
    fixture_http = NULL;
    h2_bk7258_board_runtime_deinit();
    fail("runtime", rc);
  }
  rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
      runtime, "gizclaw-e2e",
      H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI);
  if (rc)
    fail("commands", rc);
  h2_pal_task_t *runner = NULL;
  h2_pal_task_options_t options = {.name = h2_gizclaw_e2e_launcher_task_name};
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
  if (rc)
    fail("runner", rc);
  hold();
}
int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc)
    return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/gizclaw-e2e", entry, NULL);
  if (rc)
    fail("entry", rc);
  return 0;
}
