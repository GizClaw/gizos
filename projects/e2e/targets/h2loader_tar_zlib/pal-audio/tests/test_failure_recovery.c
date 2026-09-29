#include "h2_pal_audio_board_test_sdk.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <string.h>

/* Compile and execute the actual artifact's runner; no RTOS or board is faked
 * inside the production App. SDK reset is the test's terminal longjmp. */
#define main firmware_main
#ifdef H2_TEST_BOARD_SOURCE
#include H2_TEST_BOARD_SOURCE
#elif defined(H2_PAL_AUDIO_TEST_BK)
#include "projects/e2e/targets/h2loader_tar_zlib/pal-audio/bk7258_v3_202405/ap/ap_main.c"
#else
#include "projects/e2e/targets/h2loader_tar_zlib/pal-audio/amoled/main/main.c"
#endif
#undef main

static jmp_buf reset_boundary;
static int run_result, confirm_result, config_result, init_result, reboot_result;
static unsigned confirms, loader_requests, resets, delays, locks, unlocks;
static int audio_active, selected_loader;
static int mutex_cookie;
const char h2_pal_audio_e2e_runner_task_name[] = "test-audio";

static h2_pal_result_t lock_mutex(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  assert(mutex == (h2_pal_mutex_t *)&mutex_cookie);
  ++locks;
  return H2_PAL_OK;
}
static h2_pal_result_t unlock_mutex(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  assert(mutex == (h2_pal_mutex_t *)&mutex_cookie);
  ++unlocks;
  return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t sync_vtable = {
    .lock_mutex = lock_mutex, .unlock_mutex = unlock_mutex};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};
static void reset_chip(void) {
  ++resets;
  audio_active = 0;
  longjmp(reset_boundary, 1);
}
static h2_pal_result_t fallback_reboot(void *user, uint32_t reason) {
  (void)user; (void)reason;
  reset_chip();
  return H2_PAL_OK;
}
static const h2_pal_power_vtable_t power_vtable = {.reboot = fallback_reboot};
static const h2_pal_power_api_t power_api = {.vtable = &power_vtable};
void esp_restart(void) { reset_chip(); }
const h2_pal_power_api_t *h2_bk_h2loader_power_api(void) { return &power_api; }
static void delay(uint32_t ms) {
  (void)ms;
  if (++delays > 3u) longjmp(reset_boundary, 2); /* Detect the former replay loop. */
}
void vTaskDelay(uint32_t ms) { delay(ms); }
void rtos_delay_milliseconds(uint32_t ms) { delay(ms); }
int h2_pal_audio_e2e_run(const h2_pal_audio_e2e_config_t *config,
                        h2_pal_audio_e2e_result_t *out) {
  (void)config;
  memset(out, 0, sizeof(*out));
  out->failed = run_result == H2_AUDIO_OK ? 0u : 1u;
  out->passed = run_result == H2_AUDIO_OK ? H2_PAL_AUDIO_E2E_CASE_COUNT : 0u;
  for (unsigned i = 0; i < H2_PAL_AUDIO_E2E_CASE_COUNT; ++i) out->cases[i].id = "case";
  return run_result;
}
static int confirm(void) { ++confirms; return confirm_result; }
int h2_esp_h2loader_app_confirm(h2_runtime_t *value) { (void)value; return confirm(); }
int h2_bk_h2loader_confirm_current_app(h2_runtime_t *value) { (void)value; return confirm(); }
static int recovery_config(h2_loader_app_client_config_t *config) {
  memset(config, 0, sizeof(*config));
  config->h2loader_partition_id = 1u;
  config->operation_sync = &sync_api;
  config->operation_mutex = (h2_pal_mutex_t *)&mutex_cookie;
  return config_result;
}
int h2_esp_h2loader_app_commands_get_config(h2_loader_app_client_config_t *config) {
  return recovery_config(config);
}
int h2_bk_h2loader_app_commands_get_config(h2_loader_app_client_config_t *config) {
  return recovery_config(config);
}
int h2_loader_app_client_init(h2_loader_app_client_t *client,
                              const h2_loader_app_client_config_t *config) {
  memset(client, 0, sizeof(*client));
  client->loader.config.h2loader_partition_id = config->h2loader_partition_id;
  return init_result;
}
int h2_loader_reboot_h2loader_with_transition(h2_loader_t *loader,
    h2_loader_reboot_transition_fn transition, void *user) {
  (void)user;
  assert(transition == NULL && loader->config.h2loader_partition_id == 1u);
  ++loader_requests;
  if (reboot_result == H2_PAL_OK) selected_loader = 1;
  return reboot_result; /* Native reboot unexpectedly returned: test fallback. */
}
void h2_loader_deinit(h2_loader_t *loader) { (void)loader; }
int h2_runtime_init(const h2_runtime_config_t *config, h2_runtime_t **out) {
  (void)config; *out = NULL; return H2_PAL_ERR_UNAVAILABLE;
}
int h2_esp_board_runtime_config(h2_runtime_config_t *config) { (void)config; return 0; }
int h2_bk7258_board_runtime_config(h2_runtime_config_t *config) { (void)config; return 0; }
int h2_esp_board_start_entry_task(const char *name, void (*fn)(void *), void *user) {
  (void)name; (void)fn; (void)user; return 0;
}
int h2_bk7258_board_start_entry_task(const char *name, void (*fn)(void *), void *user) {
  (void)name; (void)fn; (void)user; return 0;
}
int h2_esp_target_task_policy_install(void) { return 0; }
int h2_bk_target_task_policy_install(void) { return 0; }
int h2_esp_h2loader_app_commands_prepare_serial(const h2_runtime_config_t *config,
    const char *name, uint32_t p1, uint32_t dump) {
  (void)config; (void)name; (void)p1; (void)dump; return 0;
}
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(h2_runtime_t *value,
    const char *name, uint32_t capabilities) {
  (void)value; (void)name; (void)capabilities; return 0;
}
static const test_app_description_t description = {.version = "test"};
const test_app_description_t *esp_app_get_description(void) { return &description; }
void bk_init(void) {}
uint32_t bk_misc_get_reset_reason(void) { return 0u; }

static void check(int run_rc, int confirm_rc, int cfg_rc, int init_rc, int reboot_rc) {
  static h2_runtime_t borrowed_runtime;
  runtime = &borrowed_runtime;
  provider_audio = NULL;
  run_result = run_rc; confirm_result = confirm_rc; config_result = cfg_rc;
  init_result = init_rc; reboot_result = reboot_rc;
  confirms = loader_requests = resets = delays = locks = unlocks = 0u;
  selected_loader = 0; audio_active = 1;
  switch (setjmp(reset_boundary)) {
  case 0:
    run(NULL);
    assert(0 && "failed runner returned without reset");
    break;
  case 1:
    break;
  default:
    assert(0 && "failed runner entered terminal replay");
  }
  assert(resets == 1u && !audio_active);
  assert(confirms == (run_rc == H2_AUDIO_OK ? 1u : 0u));
  assert(locks == unlocks);
  if (cfg_rc == 0 && init_rc == 0) {
    assert(loader_requests == 1u);
    assert(selected_loader == (reboot_rc == H2_PAL_OK));
  } else assert(loader_requests == 0u);
}
int main(void) {
  check(H2_PAL_ERR_INVALID_STATE, 0, 0, 0, 0);
  check(H2_PAL_ERR_INVALID_STATE, 0, H2_PAL_ERR_IO, 0, 0);
  check(H2_PAL_ERR_INVALID_STATE, 0, 0, H2_PAL_ERR_IO, 0);
  check(H2_PAL_ERR_INVALID_STATE, 0, 0, 0, H2_PAL_ERR_IO);
  check(H2_AUDIO_OK, H2_PAL_ERR_IO, 0, 0, 0);
  return 0;
}
