#include "h2_app_test_audio_fake.h"
#include "h2_app_test_crypto.h"
#include "h2_app_test_display.h"
#include "h2_app_test_fs.h"
#include "h2_app_test_modem.h"
#include "h2_app_test_periph.h"
#include "h2_app_test_power.h"
#include "h2_app_test_pref.h"
#include "h2_app_test_time.h"
#include "h2_app_test_wifi.h"
#include "h2_libco.h"
#include "h2_runtime.h"
#include "h2_runtime_test.h"
#include "testing_pal_support.h"
#include <string.h>
#define OK(x) assert((x) == H2_PAL_OK)

typedef struct environment {
  test_memory_t memory;
  h2_pal_mem_api_t mem;
  h2_app_test_time_t clock;
  h2_app_test_pref_t pref;
  h2_app_test_periph_t periph;
  h2_app_test_power_t power;
  h2_app_test_display_t display;
  h2_app_test_wifi_t wifi;
  h2_app_test_modem_t modem;
  h2_app_test_fs_t fs;
  h2_app_test_crypto_t crypto;
  h2_app_test_audio_fake_t audio;
  h2_libco_t *executor;
  h2_runtime_t *runtime;
  bool complete;
} environment_t;
static void *alloc(void *u, size_t size) {
  return test_alloc(&((environment_t *)u)->memory, size);
}
static void release(void *u, void *p) {
  test_free(&((environment_t *)u)->memory, p);
}
static uint64_t now(void *u) {
  return ((environment_t *)u)->clock.monotonic_ms;
}
static int mappings(void *u, h2_runtime_component_t filter,
                    h2_runtime_component_mapping_cb_t cb, void *ctx) {
  (void)u;
  const h2_runtime_component_t kinds[] = {H2_RUNTIME_COMPONENT_BUTTON,
                                          H2_RUNTIME_COMPONENT_BATTERY,
                                          H2_RUNTIME_COMPONENT_PWM_SWITCH};
  for (uint32_t i = 0; i < 3; ++i)
    if (filter == H2_RUNTIME_COMPONENT_NONE || filter == kinds[i]) {
      h2_runtime_component_mapping_entry_t entry = {11u + i, 91u + i};
      int rc = cb(ctx, &entry);
      if (rc)
        return rc;
    }
  return 0;
}
static const h2_runtime_component_mapper_vtable_t mapper_vtable = {
    .list = mappings};
static const h2_runtime_component_mapper_t mapper = {.vtable = &mapper_vtable};
static h2_runtime_config_t runtime_config(environment_t *e) {
  return (h2_runtime_config_t){
      .board = "app-test",
      .target = "memory",
      .chip = "host",
      .firmware_info = h2_pal_unsupported_firmware_info_api(),
      .mem = &e->mem,
      .log = h2_pal_unsupported_log_api(),
      .time = h2_libco_time_api(e->executor),
      .timer = h2_pal_unsupported_timer_api(),
      .task = h2_libco_task_api(e->executor),
      .queue = h2_libco_queue_api(e->executor),
      .sync = h2_libco_sync_api(e->executor),
      .fs = &e->fs.api,
      .disk = h2_pal_unsupported_disk_api(),
      .pref = &e->pref.api,
      .crypto = &e->crypto.api,
      .http = h2_pal_unsupported_http_api(),
      .net = h2_pal_unsupported_net_api(),
      .netif = h2_pal_unsupported_netif_api(),
      .mqtt = h2_pal_unsupported_mqtt_api(),
      .webrtc = h2_pal_unsupported_webrtc_api(),
      .wifi_sta = &e->wifi.api,
      .wifi_ap = h2_pal_unsupported_wifi_ap_api(),
      .wifi_csi = h2_pal_unsupported_wifi_csi_api(),
      .wifi_settings = &e->wifi.settings,
      .ble_host = h2_pal_unsupported_ble_host_api(),
      .modem = &e->modem.api,
      .power = &e->power.api,
      .display = &e->display.api,
      .audio = &e->audio.api,
      .audio_decoder = h2_pal_unsupported_audio_decoder_api(),
      .periph = &e->periph.api,
      .button = &e->periph.button,
      .touch = h2_pal_unsupported_touch_api(),
      .buzzer = h2_pal_unsupported_buzzer_api(),
      .nfc = h2_pal_unsupported_nfc_api(),
      .nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api(),
      .imu = h2_pal_unsupported_imu_api(),
      .gpio_irq = h2_pal_unsupported_gpio_irq_api(),
      .led = h2_pal_unsupported_led_api(),
      .switch_api = h2_pal_unsupported_switch_api(),
      .pwm_switch = &e->periph.pwm,
      .input = &e->periph.input,
      .system_event = h2_pal_unsupported_system_event_api(),
      .video_decoder = h2_pal_unsupported_video_decoder_api(),
      .component_mapper = &mapper,
      .event_queue_capacity = 4u,
  };
}
/* A real cooperative worker must suspend on retry; no inline task-name bypass.
 */
static void settings_worker(void *u) {
  environment_t *e = u;
  h2_runtime_test_control_t *control = NULL;
  OK(h2_runtime_test_control_open(e->runtime, &control));
  OK(h2_runtime_test_button_action(control, 11u, 100u, 120u, 120u));
  h2_runtime_button_state_t button;
  OK(h2_runtime_component_state_button(e->runtime, 11u, &button));
  assert(!button.pressed && button.updated_at_ms == 120u);
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {.payload = payload,
                              .payload_capacity = sizeof(payload)};
  OK(h2_runtime_poll_event(e->runtime, &event));
  assert(event.kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION &&
         event.component_id == 11u);
  h2_runtime_button_action_event_t action;
  memcpy(&action, event.payload, sizeof(action));
  assert(action.pressed_at_ms == 100u && action.released_at_ms == 120u);
  h2_runtime_test_control_close(control);
  h2_pal_pref_namespace_t *ns = NULL;
  OK(h2_pal_pref_open(e->runtime->pref, "settings", H2_PAL_PREF_OPEN_READ_WRITE,
                      &ns));
  OK(ns->set_u32(ns, "volume", 37u));
  assert(ns->commit(ns) == H2_PAL_ERR_IO);
  OK(h2_pal_time_sleep_ms(e->runtime->time, 10u));
  OK(ns->commit(ns));
  OK(ns->close(ns));
  OK(h2_pal_audio_set_speaker_volume_percent(e->runtime->audio, 37u));
  OK(h2_pal_display_open(e->runtime->display));
  OK(h2_pal_display_set_brightness_percent(e->runtime->display, 60u));
  h2_pal_periph_id_t id;
  OK(h2_runtime_periph_id(e->runtime, 13u, &id));
  OK(h2_pal_pwm_switch_set_duty(e->runtime->pwm_switch, id, 4000u));
  e->complete = true;
}
static void cleanup_runtime(void *user) {
  environment_t *e = user;
  h2_runtime_deinit(e->runtime);
  e->runtime = NULL;
}

int main(void) {
  environment_t e = {0};
  e.mem = (h2_pal_mem_api_t){&e.memory, &test_memory_vtable};
  h2_app_test_time_init(&e.clock, 100u);
  OK(h2_app_test_pref_init(&e.pref, &e.mem));
  OK(h2_app_test_fs_init(&e.fs, &e.mem));
  OK(h2_app_test_audio_fake_init(&e.audio, &e.mem));
  h2_app_test_power_init(&e.power);
  h2_app_test_display_init(&e.display);
  h2_app_test_wifi_init(&e.wifi);
  h2_app_test_modem_init(&e.modem);
  h2_app_test_crypto_init(&e.crypto);
  h2_app_test_periph_init(&e.periph);
  const h2_pal_periph_type_t types[] = {H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
                                        H2_PAL_PERIPH_TYPE_BATTERY,
                                        H2_PAL_PERIPH_TYPE_PWM_SWITCH};
  for (uint32_t i = 0; i < 3; ++i) {
    h2_pal_periph_info_t info = {
        .id = 91u + i, .type = types[i], .name = "test"};
    h2_app_test_periph_entry_t *entry;
    OK(h2_app_test_periph_add(&e.periph, &info, &entry));
  }
  e.periph.entries[1].battery = (h2_pal_battery_reading_t){
      .flags = H2_PAL_BATTERY_HAS_PERCENT_X100, .percent_x100 = 7600u};
  const h2_libco_config_t executor_config = {.user = &e,
                                             .alloc = alloc,
                                             .free = release,
                                             .now_ms = now,
                                             .time_source = &e.clock.api};
  assert(h2_libco_create(&executor_config, &e.executor) == H2_LIBCO_OK);
  h2_runtime_config_t config = runtime_config(&e);
  OK(h2_runtime_init(&config, &e.runtime));
  e.pref.commit = (h2_app_test_fault_t){.result = H2_PAL_ERR_IO, .remaining = 1u, .calls = 0u};
  h2_pal_task_t *task = NULL;
  h2_pal_task_options_t options = {.name = "test/settings",
                                   .min_stack_size = 65536u};
  OK(h2_pal_task_start(e.runtime->task, &options, settings_worker, &e, &task));
  size_t resumed = 0;
  assert(h2_libco_schedule(e.executor, 8u, &resumed) == H2_LIBCO_OK);
  assert(!e.complete);
  OK(h2_app_test_time_advance(&e.clock, 9u));
  assert(h2_libco_schedule(e.executor, 8u, &resumed) == H2_LIBCO_OK);
  assert(!e.complete);
  OK(h2_app_test_time_advance(&e.clock, 1u));
  assert(h2_libco_schedule(e.executor, 8u, &resumed) == H2_LIBCO_OK);
  assert(e.complete);
  OK(h2_pal_task_join(e.runtime->task, task));
  task = NULL;
  assert(e.audio.volume_percent == 37u && e.display.brightness_percent == 60u &&
         e.periph.entries[2].duty_x100 == 4000u);
  h2_pal_pref_namespace_t *reader;
  OK(h2_pal_pref_open(e.runtime->pref, "settings", H2_PAL_PREF_OPEN_READ_ONLY,
                      &reader));
  uint32_t value = 0;
  OK(reader->get_u32(reader, "volume", &value));
  assert(value == 37u);
  OK(reader->close(reader));
  OK(h2_pal_display_close(e.runtime->display));
  OK(h2_pal_task_start(h2_libco_task_api(e.executor), &options, cleanup_runtime,
                       &e, &task));
  assert(h2_libco_schedule(e.executor, 8u, &resumed) == H2_LIBCO_OK);
  assert(e.runtime == NULL);
  OK(h2_pal_task_join(h2_libco_task_api(e.executor), task));
  assert(h2_libco_destroy(&e.executor) == H2_LIBCO_OK);
  OK(h2_app_test_audio_fake_deinit(&e.audio));
  OK(h2_app_test_pref_deinit(&e.pref));
  OK(h2_app_test_fs_deinit(&e.fs));
  assert(e.memory.live == 0u);
  return 0;
}
