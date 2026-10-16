#include "h2/pal/h2_pal_unsupported.h"
#include "h2_lua.h"
#include "h2_lua_job.h"
#include "h2_web_platform.h"
#include <assert.h>
#include <emscripten.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic unsigned timer_calls;
static int deny_timer(void *user, const h2_pal_timer_config_t *config,
                      h2_pal_timer_t **out) {
  (void)user; (void)config;
  *out = NULL;
  atomic_fetch_add(&timer_calls, 1u);
  return H2_PAL_ERR_IO;
}
static h2_pal_timer_vtable_t timer_vtable;
static const h2_pal_timer_api_t timer_api = {.vtable = &timer_vtable};

static h2_lua_job_status_t wait_for(h2_lua_host_t *host, h2_lua_job_id_t id,
                                    h2_lua_job_state_t wanted,
                                    const h2_pal_time_api_t *time) {
  const double deadline = emscripten_get_now() + 10000.0;
  double last_progress = emscripten_get_now();
  uint64_t resumes = 0;
  h2_lua_job_status_t status;
  for (;;) {
    assert(h2_lua_job_get_status(host, id, &status) == H2_PAL_OK);
    if (status.state == wanted) return status;
    if (status.state >= H2_LUA_JOB_SUCCEEDED)
      fprintf(stderr, "LUA unexpected state=%d resumes=%llu timers=%u message=%s\n",
          status.state, (unsigned long long)status.resume_count,
          atomic_load(&timer_calls), status.message);
    assert(status.state < H2_LUA_JOB_SUCCEEDED);
    assert(emscripten_get_now() < deadline);
    if (status.resume_count != resumes) {
      resumes = status.resume_count;
      last_progress = emscripten_get_now();
    }
    assert(emscripten_get_now() - last_progress < 2000.0);
    assert(h2_pal_time_sleep_ms(time, 2u) == H2_PAL_OK);
  }
}

int main(void) {
  h2_web_platform_t *platform = h2_web_platform_create(
      &(h2_web_platform_config_t){64, 64});
  assert(platform != NULL);
  timer_vtable = *h2_pal_unsupported_timer_api()->vtable;
  timer_vtable.create = deny_timer;
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
  rcfg.fs = h2_pal_unsupported_fs_api();
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
  rcfg.timer = &timer_api;
  h2_runtime_t *runtime = NULL;
  assert(h2_runtime_init(&rcfg, &runtime) == H2_PAL_OK);
  h2_lua_host_t *host = NULL;
  assert(h2_lua_host_create(&(h2_lua_host_config_t){.runtime = runtime,
      .worker_count = 1, .max_jobs = 1, .execution_timeout_ms = 10000}, &host) == H2_PAL_OK);
  assert(h2_lua_host_start(host) == H2_PAL_OK);
  static const char source[] = "local r=require('runtime');for i=1,240 do r.sleep(i%7+1) end;return 240";
  h2_lua_job_id_t id;
  assert(h2_lua_job_submit_text(host, NULL, "progress", (const uint8_t *)source,
      sizeof(source)-1, NULL, 0, &id) == H2_PAL_OK);
  h2_lua_job_status_t status = wait_for(host, id, H2_LUA_JOB_SUCCEEDED, runtime->time);
  assert(status.resume_count >= 241 && atomic_load(&timer_calls) == 0);
  char result[16]; size_t size; int present;
  assert(h2_lua_job_get_result(host, id, result, sizeof(result), &size, &present) == H2_PAL_OK);
  assert(present && size == 3 && strcmp(result, "240") == 0);
  assert(h2_lua_job_release(host, id) == H2_PAL_OK);
  puts("LUA worker_deadline_progress=PASS");
  static const char sleeping[] = "require('runtime').sleep(60000);error('unexpected wake')";
  assert(h2_lua_job_submit_text(host, NULL, "stop", (const uint8_t *)sleeping,
      sizeof(sleeping)-1, NULL, 0, &id) == H2_PAL_OK);
  (void)wait_for(host, id, H2_LUA_JOB_WAITING, runtime->time);
  const double began = emscripten_get_now();
  assert(h2_lua_host_stop(host) == H2_PAL_OK);
  assert(h2_lua_host_join(host) == H2_PAL_OK);
  assert(emscripten_get_now()-began < 2000.0);
  assert(h2_lua_job_get_status(host, id, &status) == H2_PAL_OK);
  assert(status.state == H2_LUA_JOB_STOPPED && atomic_load(&timer_calls) == 0);
  assert(h2_lua_host_destroy_checked(host) == H2_PAL_OK);
  puts("LUA worker_deadline_stop=PASS");
  assert(h2_runtime_destroy(&runtime) == H2_PAL_OK && runtime == NULL);
  assert(h2_web_platform_destroy(platform) == H2_PAL_OK);
  puts("LUA teardown=PASS");
  return 0;
}
