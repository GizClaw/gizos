#include "h2_lua.h"
#include "h2_lua_apps.h"
#include "h2_lua_job.h"
#include "h2_web_fs.h"
#include "h2_web_main_thread.h"
#include "h2_web_platform.h"

#include <assert.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

/* clang-format off */
EM_JS(void, fixture_setup,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['pointer', 'u32'], 'i32', (url, capacity) => {
    stringToUTF8(location.origin + '/web_probe.tar.zlib', url, capacity);
    return Number(sessionStorage.getItem('h2-scoped-phase') || 0);
  });
});
/* clang-format on */

static h2_runtime_t *runtime_create(h2_web_platform_t *platform,
                                    const h2_pal_fs_api_t *fs);

static void run_job(h2_runtime_t *runtime, const char *entry, int expected) {
  h2_lua_host_t *host = NULL;
  const h2_lua_host_config_t config = {
      .runtime = runtime,
      .worker_count = 1,
      .max_jobs = 1,
      .execution_timeout_ms = 10000,
      .storage = {.fs = runtime->fs, .root = "/data/gizclaw/lua"}};
  assert(h2_lua_host_create(&config, &host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  h2_lua_job_id_t id;
  if (entry != NULL) {
    assert(h2_lua_job_submit_path(host, "web_probe", "installed", entry, NULL,
                                  0, &id) == H2_PAL_OK);
  } else {
    static const char verify[] =
        "assert(require('storage').read_file('visits')=='2');"
        "return require('kv').get('visits')";
    assert(h2_lua_job_submit_text(host, "web_probe", "retained KV",
                                  (const uint8_t *)verify, sizeof(verify) - 1u,
                                  NULL, 0, &id) == H2_PAL_OK);
  }
  h2_lua_job_status_t status;
  const double deadline = emscripten_get_now() + 15000.0;
  do {
    assert(h2_lua_job_get_status(host, id, &status) == H2_PAL_OK);
    if (status.state >= H2_LUA_JOB_SUCCEEDED)
      break;
    assert(emscripten_get_now() < deadline);
    assert(h2_pal_time_sleep_ms(runtime->time, 2u) == H2_PAL_OK);
  } while (1);
  if (status.state != H2_LUA_JOB_SUCCEEDED)
    fprintf(stderr, "SCOPED Lua state=%d message=%s\n", status.state,
            status.message);
  assert(status.state == H2_LUA_JOB_SUCCEEDED);
  char value[16];
  size_t size;
  int present;
  assert(h2_lua_job_get_result(host, id, value, sizeof(value), &size,
                               &present) == H2_PAL_OK);
  assert(present && size == 1u && value[0] == '0' + expected);
  assert(h2_lua_host_stop(host) == H2_PAL_OK);
  assert(h2_lua_host_join(host) == H2_PAL_OK);
  assert(h2_lua_host_destroy_checked(host) == H2_PAL_OK);
}

int main(void) {
  h2_web_platform_t *platform =
      h2_web_platform_create(&(h2_web_platform_config_t){64, 64});
  assert(platform != NULL);
  char url[H2_LUA_APP_URL_MAX];
  const int phase =
      h2_web_main_call(fixture_setup,
                       (const void *[]){&(char *){url}, &(size_t){sizeof(url)}})
          .i32;
  assert(phase >= 0 && phase <= 2);
  const char *readonly[] = {"/data/lua", "/data/assets"};
  h2_web_fs_t *fs = NULL;
  const h2_web_fs_config_t fs_config = {.persistent_root = "/data/gizclaw",
                                        .readonly_roots = readonly,
                                        .readonly_root_count = 2u};
  assert(h2_web_fs_open(platform, &fs_config, &fs) == H2_PAL_OK);
  const h2_pal_fs_api_t *api = h2_web_fs_api(fs);
  h2_pal_fs_stat_t stat;
  assert(h2_pal_fs_stat(api, "/data", &stat) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_fs_mkdir(api, "/data") == H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_fs_mkdir(api, "/data/outside") == H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_fs_mkdir(api, "/data/lua/new") == H2_PAL_ERR_UNSUPPORTED);
  h2_lua_apps_t *apps = NULL;
  const h2_lua_apps_config_t catalog = {
      .allocator = h2_web_platform_mem_api(),
      .fs = api,
      .sync = h2_web_platform_sync_api(platform),
      .install_root = "/data/gizclaw/lua_apps"};
  assert(h2_lua_apps_create(&catalog, &apps) == H2_PAL_OK);
  h2_lua_app_info_t info;
  if (phase == 0) {
    const int result =
        h2_lua_apps_install(apps, h2_web_platform_http_api(platform),
                            (h2_lua_app_source_t){url, strlen(url)}, NULL,
                            10000u, NULL, NULL, &info);
    printf("SCOPED HTTP_install rc=%d\n", result);
    assert(result == H2_PAL_OK);
    puts("SCOPED HTTP_install=PASS");
  }
  h2_runtime_t *runtime = runtime_create(platform, api);
  h2_lua_app_info_t listed;
  size_t listed_count = 99u;
  assert(h2_lua_apps_list(apps, &listed, 1u, &listed_count) == H2_PAL_OK);
  assert(listed_count == (phase <= 1 ? 1u : 0u));
  if (phase <= 1) {
    assert(strcmp(listed.app_id, "web_probe") == 0);
    assert(h2_lua_apps_get(apps, "web_probe", &info) == H2_PAL_OK);
    assert(strcmp(info.version, "1.0.0") == 0);
    assert(strncmp(info.entry, "/data/gizclaw/lua_apps/", 23u) == 0);
    run_job(runtime, info.entry, phase + 1);
    if (phase == 1) {
      assert(h2_lua_apps_uninstall(apps, "web_probe") == H2_PAL_OK);
      assert(h2_pal_fs_stat(api, info.entry, &stat) == H2_PAL_ERR_NOT_FOUND);
      puts("SCOPED restored_app_and_KV=PASS");
    }
  } else {
    assert(h2_lua_apps_get(apps, "web_probe", &info) == H2_PAL_ERR_NOT_FOUND);
    run_job(runtime, NULL, 2);
    puts("SCOPED uninstall_keeps_KV=PASS");
  }
  assert(h2_runtime_destroy(&runtime) == H2_PAL_OK && runtime == NULL);
  assert(h2_lua_apps_destroy(apps) == H2_PAL_OK);
  assert(h2_web_fs_close(fs) == H2_PAL_OK);
  assert(h2_web_platform_destroy(platform) == H2_PAL_OK);
  if (phase == 0)
    puts("SCOPED phase0_saved");
  else if (phase == 1)
    puts("SCOPED phase1_saved");
  else
    puts("SCOPED teardown=PASS");
  return 0;
}

static h2_runtime_t *runtime_create(h2_web_platform_t *platform,
                                    const h2_pal_fs_api_t *fs) {
  h2_runtime_config_t rcfg = {0};
  rcfg.board = "browser";
  rcfg.target = "webassembly";
  rcfg.chip = "wasm32";
  rcfg.firmware_info = h2_pal_unsupported_firmware_info_api();
  rcfg.mem = h2_web_platform_mem_api();
  rcfg.log = h2_web_platform_log_api();
  rcfg.time = h2_web_platform_time_api(platform);
  rcfg.timer = h2_web_platform_timer_api(platform);
  rcfg.task = h2_web_platform_task_api(platform);
  rcfg.queue = h2_web_platform_queue_api(platform);
  rcfg.sync = h2_web_platform_sync_api(platform);
  rcfg.fs = fs;
  rcfg.disk = h2_pal_unsupported_disk_api();
  rcfg.pref = h2_web_platform_pref_api(platform);
  rcfg.crypto = h2_web_platform_crypto_api(platform);
  rcfg.http = h2_web_platform_http_api(platform);
  rcfg.net = h2_pal_unsupported_net_api();
  rcfg.netif = h2_web_platform_netif_api(platform);
  rcfg.mqtt = h2_pal_unsupported_mqtt_api();
  rcfg.webrtc = h2_web_platform_webrtc_api(platform);
  rcfg.wifi_sta = h2_pal_unsupported_wifi_sta_api();
  rcfg.wifi_ap = h2_pal_unsupported_wifi_ap_api();
  rcfg.wifi_csi = h2_pal_unsupported_wifi_csi_api();
  rcfg.wifi_settings = h2_pal_unsupported_wifi_settings_api();
  rcfg.ble_host = h2_pal_unsupported_ble_host_api();
  rcfg.modem = h2_pal_unsupported_modem_api();
  if (h2_web_platform_fake_wifi_sta_api(platform) != NULL) {
    rcfg.wifi_sta = h2_web_platform_fake_wifi_sta_api(platform);
    rcfg.wifi_settings = h2_web_platform_fake_wifi_settings_api(platform);
    rcfg.modem = h2_web_platform_fake_modem_api(platform);
  }
  rcfg.power = h2_pal_unsupported_power_api();
  rcfg.display = h2_web_platform_display_api(platform);
  rcfg.audio = h2_web_platform_audio_api(platform);
  rcfg.audio_decoder = h2_web_platform_audio_decoder_api(platform);
  rcfg.periph = h2_pal_unsupported_periph_api();
  rcfg.button = h2_pal_unsupported_button_api();
  rcfg.touch = h2_web_platform_touch_api(platform);
  rcfg.buzzer = h2_pal_unsupported_buzzer_api();
  rcfg.nfc = h2_pal_unsupported_nfc_api();
  rcfg.nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api();
  rcfg.imu = h2_pal_unsupported_imu_api();
  rcfg.gpio_irq = h2_pal_unsupported_gpio_irq_api();
  rcfg.led = h2_pal_unsupported_led_api();
  rcfg.switch_api = h2_pal_unsupported_switch_api();
  rcfg.pwm_switch = h2_pal_unsupported_pwm_switch_api();
  rcfg.input = h2_pal_unsupported_input_api();
  rcfg.system_event = h2_web_platform_system_event_api(platform);
  rcfg.video_decoder = h2_web_platform_video_decoder_api(platform);
  rcfg.component_mapper = NULL;
  rcfg.event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(&rcfg, &runtime) == H2_PAL_OK);
  return runtime;
}
