#include "bk_private/bk_init.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_target_task_policy.h"
#include "h2_gizclaw_e2e.h"
#include "h2_gizclaw_e2e_bk_config.h"
#include "h2_gizclaw_e2e_task_names.h"
#include <os/os.h>
#include <stdio.h>
#include <string.h>

extern const unsigned char h2_gizclaw_e2e_pcm[];
extern const size_t h2_gizclaw_e2e_pcm_size;
static h2_runtime_t *runtime;
static h2_gizclaw_e2e_result_t result;
static h2_gizclaw_e2e_config_t app_config;
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
static void run(void *user) {
  (void)user;
  int rc;
  if ((!h2_gizclaw_e2e_fixture_key()[0] ||
       !h2_gizclaw_e2e_fixture_profile()[0] || !h2_gizclaw_e2e_fixture_value()[0])) {
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
  const h2_gizclaw_e2e_bk_config_t *settings = h2_gizclaw_e2e_bk_config();
  app_config = (h2_gizclaw_e2e_config_t){
      .server_endpoint = settings->server_endpoint,
      .registration_token = settings->registration_token,
      .app_config_key = h2_gizclaw_e2e_fixture_key(),
      .expected_runtime_profile = h2_gizclaw_e2e_fixture_profile(),
      .app_config_expected_value = {h2_gizclaw_e2e_fixture_value(),
                                   strlen(h2_gizclaw_e2e_fixture_value())},
      .voice_audio = runtime->audio,
      .voice_pcm_s16le_16khz_mono = h2_gizclaw_e2e_pcm,
      .voice_pcm_len = h2_gizclaw_e2e_pcm_size,
      .device_api_url = "https://ap.e2e.gizclaw.com",
      .device_audio_url = "https://raw.githubusercontent.com/GizClaw/gizos/"
                          "cf8dbdeba320984fc57ddba670dcf55237aa39cf/projects/"
                          "e2e/apps/gizclaw/data/playback_tone_32s_v1.ogg",
      .device_real_audio = true,
      .suites = H2_GIZCLAW_E2E_SUITE_ALL};
  rc = h2_gizclaw_e2e_run(runtime, &app_config, &result);
  printf("H2_GIZCLAW_E2E stage=summary platform=bk7258 endpoint=%.*s "
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
  int confirm = rc == 0 && result.complete && !result.cleanup_rc &&
                        !result.retained_resources
                    ? h2_bk_h2loader_confirm_current_app(runtime)
                    : H2_PAL_ERR_INVALID_STATE;
  printf("H2_GIZCLAW_READY board=bk7258 rc=%d confirm=%d\n", rc, confirm);
  fflush(stdout);
  hold();
}
static void entry(void *user) {
  (void)user;
  h2_runtime_config_t config = {0};
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc)
    fail("board", rc);
  rc = h2_runtime_init(&config, &runtime);
  if (rc)
    fail("runtime", rc);
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
