#include "h2/pal/h2_pal_unsupported.h"
#include "h2_runtime.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "Web netif test failed at line %d: %s\n", __LINE__,     \
              #condition);                                                     \
      emscripten_force_exit(1);                                                \
    }                                                                          \
  } while (0)

EM_JS(void, test_set_online, (int online, int notify),
      { globalThis.h2TestNetif.set(!!online, !!notify); });
EM_JS(void, test_set_present, (int present),
      { globalThis.h2TestNetif.present(!!present); });
EM_JS(int, test_listener_count, (),
      { return globalThis.h2TestNetif.listeners(); });

typedef struct test_events {
  int count;
  h2_pal_netif_default_changed_t last;
} test_events_t;

static int test_record_event(void *user, const h2_pal_system_event_t *event) {
  test_events_t *events = user;
  CHECK(event->type == H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED);
  CHECK(event->payload_size == sizeof(events->last));
  memcpy(&events->last, event->payload, sizeof(events->last));
  CHECK(h2_pal_netif_default_changed_is_valid(&events->last));
  ++events->count;
  return H2_PAL_OK;
}

static int test_count_netif(void *user, const h2_pal_netif_ref_t *ref,
                            const h2_pal_netif_status_t *status) {
  int *count = user;
  CHECK(ref != NULL && status != NULL);
  ++*count;
  return 0;
}

static void test_pump(h2_web_platform_t *platform) {
  CHECK(h2_web_platform_pump(platform, 8u, NULL) == H2_PAL_OK);
}

static int test_is_browser(const h2_pal_netif_ref_t *ref) {
  return ref->type == H2_PAL_NETIF_REF_NAME &&
         ref->kind == H2_PAL_NETIF_KIND_HOST && strcmp(ref->name, "browser") == 0;
}

static void test_status_contract(h2_web_platform_t *platform) {
  const h2_pal_netif_api_t *netif = h2_web_platform_netif_api(platform);
  const h2_pal_netif_ref_t default_ref = h2_pal_netif_default_ref();
  h2_pal_netif_status_t status;
  test_set_online(1, 0);
  CHECK(h2_pal_netif_get_status(netif, &default_ref, &status) == H2_PAL_OK);
  CHECK(test_is_browser(&status.ref));
  CHECK(status.kind == H2_PAL_NETIF_KIND_HOST);
  CHECK(status.flags == (H2_PAL_NETIF_FLAG_UP | H2_PAL_NETIF_FLAG_LINK_UP |
                         H2_PAL_NETIF_FLAG_DEFAULT_ROUTE));
  CHECK(h2_pal_netif_status_is_usable(&status));
  // Nothing the browser hides may be invented.
  const h2_pal_net_addr_t zero_addr = {0};
  CHECK(memcmp(&status.ipv4, &zero_addr, sizeof(zero_addr)) == 0);
  CHECK(memcmp(&status.gateway4, &zero_addr, sizeof(zero_addr)) == 0);
  CHECK(memcmp(&status.ipv6, &zero_addr, sizeof(zero_addr)) == 0);
  CHECK(status.mtu == 0u && status.mac_valid == 0u && status.dns_count == 0u);

  const h2_pal_netif_ref_t browser = status.ref;
  h2_pal_netif_ref_t found;
  const h2_pal_netif_filter_t any = {0};
  CHECK(h2_pal_netif_find(netif, &any, &found) == H2_PAL_OK);
  CHECK(h2_pal_netif_ref_equal(&found, &browser));
  const h2_pal_netif_filter_t host = {.kind = H2_PAL_NETIF_KIND_HOST};
  CHECK(h2_pal_netif_find(netif, &host, &found) == H2_PAL_OK);
  const h2_pal_netif_kind_t absent[] = {
      H2_PAL_NETIF_KIND_WIFI_STA, H2_PAL_NETIF_KIND_WIFI_AP,
      H2_PAL_NETIF_KIND_MODEM_DATA, H2_PAL_NETIF_KIND_ETHERNET,
      H2_PAL_NETIF_KIND_LOOPBACK};
  for (size_t index = 0u; index < sizeof(absent) / sizeof(absent[0]);
       ++index) {
    const h2_pal_netif_filter_t filter = {.kind = absent[index]};
    CHECK(h2_pal_netif_find(netif, &filter, &found) == H2_PAL_ERR_NOT_FOUND);
    int count = 0;
    CHECK(h2_pal_netif_list(netif, &filter, test_count_netif, &count) ==
          H2_PAL_OK);
    CHECK(count == 0);
  }
  const h2_pal_netif_filter_t by_id = {.id = 1u};
  CHECK(h2_pal_netif_find(netif, &by_id, &found) == H2_PAL_ERR_NOT_FOUND);
  int count = 0;
  CHECK(h2_pal_netif_list(netif, NULL, test_count_netif, &count) == H2_PAL_OK);
  CHECK(count == 1);

  h2_pal_netif_dns_server_t dns[2];
  size_t dns_count = 7u;
  CHECK(h2_pal_netif_get_dns_servers(netif, &browser, dns, 2u, &dns_count) ==
        H2_PAL_ERR_UNSUPPORTED);
  CHECK(dns_count == 0u);
  CHECK(h2_pal_netif_set_default(netif, &browser) == H2_PAL_ERR_UNSUPPORTED);
  h2_pal_netif_ref_t wifi = browser;
  wifi.kind = H2_PAL_NETIF_KIND_WIFI_STA;
  memcpy(wifi.name, "wlan0", sizeof("wlan0"));
  CHECK(h2_pal_netif_set_default(netif, &wifi) == H2_PAL_ERR_NOT_FOUND);
  CHECK(h2_pal_netif_get_status(netif, &wifi, &status) ==
        H2_PAL_ERR_NOT_FOUND);

  // Offline: no default route, the host path stays enumerable but unusable.
  test_set_online(0, 0);
  CHECK(h2_pal_netif_get_status(netif, &default_ref, &status) ==
        H2_PAL_ERR_NOT_FOUND);
  CHECK(h2_pal_netif_get_status(netif, &browser, &status) == H2_PAL_OK);
  CHECK(status.flags == H2_PAL_NETIF_FLAG_UP);
  CHECK(!h2_pal_netif_status_is_usable(&status));
  test_set_online(1, 0);
}

static void test_events(h2_web_platform_t *platform) {
  const h2_pal_system_event_api_t *events_api =
      h2_web_platform_system_event_api(platform);
  test_events_t events = {0};
  h2_pal_system_event_subscription_t *subscription = NULL;
  CHECK(h2_pal_system_event_init(events_api) == H2_PAL_OK);
  CHECK(h2_pal_system_event_subscribe(
            events_api, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
            test_record_event, &events, &subscription) == H2_PAL_OK);
  test_pump(platform);
  CHECK(events.count == 0);

  test_set_online(0, 1);
  test_pump(platform);
  CHECK(events.count == 1);
  CHECK(events.last.previous_valid == 1u && events.last.current_valid == 0u);
  CHECK(test_is_browser(&events.last.previous));

  // A repeated browser notification without a state change is deduplicated.
  test_set_online(0, 1);
  test_pump(platform);
  CHECK(events.count == 1);

  test_set_online(1, 1);
  test_pump(platform);
  CHECK(events.count == 2);
  CHECK(events.last.previous_valid == 0u && events.last.current_valid == 1u);
  CHECK(test_is_browser(&events.last.current));

  // Offline then online inside one browser turn cancels out.
  test_set_online(0, 1);
  test_set_online(1, 1);
  test_pump(platform);
  CHECK(events.count == 2);

  h2_pal_system_event_unsubscribe(events_api, subscription);
  test_set_online(0, 1);
  test_pump(platform);
  CHECK(events.count == 2);
  test_set_online(1, 1);
  test_pump(platform);
  h2_pal_system_event_deinit(events_api);
}

static h2_runtime_config_t test_runtime_config(h2_web_platform_t *platform) {
  h2_runtime_config_t config = {0};
  config.board = "browser";
  config.target = "webassembly";
  config.chip = "wasm32";
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = h2_web_platform_mem_api();
  config.log = h2_web_platform_log_api();
  config.time = h2_web_platform_time_api(platform);
  config.timer = h2_web_platform_timer_api(platform);
  config.task = h2_web_platform_task_api(platform);
  config.queue = h2_web_platform_queue_api(platform);
  config.sync = h2_web_platform_sync_api(platform);
  config.fs = h2_pal_unsupported_fs_api();
  config.disk = h2_pal_unsupported_disk_api();
  config.pref = h2_pal_unsupported_pref_api();
  config.crypto = h2_pal_unsupported_crypto_api();
  config.http = h2_pal_unsupported_http_api();
  config.net = h2_pal_unsupported_net_api();
  config.netif = h2_web_platform_netif_api(platform);
  config.mqtt = h2_pal_unsupported_mqtt_api();
  config.webrtc = h2_pal_unsupported_webrtc_api();
  config.wifi_sta = h2_pal_unsupported_wifi_sta_api();
  config.wifi_ap = h2_pal_unsupported_wifi_ap_api();
  config.wifi_csi = h2_pal_unsupported_wifi_csi_api();
  config.wifi_settings = h2_pal_unsupported_wifi_settings_api();
  config.ble_host = h2_pal_unsupported_ble_host_api();
  config.modem = h2_pal_unsupported_modem_api();
  config.power = h2_pal_unsupported_power_api();
  config.display = h2_pal_unsupported_display_api();
  config.audio = h2_pal_unsupported_audio_api();
  config.audio_decoder = h2_pal_unsupported_audio_decoder_api();
  config.periph = h2_pal_unsupported_periph_api();
  config.button = h2_pal_unsupported_button_api();
  config.touch = h2_pal_unsupported_touch_api();
  config.buzzer = h2_pal_unsupported_buzzer_api();
  config.nfc = h2_pal_unsupported_nfc_api();
  config.nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api();
  config.imu = h2_pal_unsupported_imu_api();
  config.gpio_irq = h2_pal_unsupported_gpio_irq_api();
  config.led = h2_pal_unsupported_led_api();
  config.switch_api = h2_pal_unsupported_switch_api();
  config.pwm_switch = h2_pal_unsupported_pwm_switch_api();
  config.input = h2_pal_unsupported_input_api();
  config.system_event = h2_web_platform_system_event_api(platform);
  config.video_decoder = h2_pal_unsupported_video_decoder_api();
  config.event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
  return config;
}

static void test_runtime_delivery(h2_web_platform_t *platform) {
  const h2_runtime_config_t config = test_runtime_config(platform);
  h2_runtime_t *runtime = NULL;
  CHECK(h2_runtime_init(&config, &runtime) == H2_PAL_OK);
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  h2_runtime_event_t event = {
      .payload = payload,
      .payload_capacity = sizeof(payload),
  };
  const h2_runtime_system_event_netif_default_changed_t *change =
      (const h2_runtime_system_event_netif_default_changed_t *)payload;
  const int expected_online[] = {0, 1};
  for (size_t index = 0u; index < 2u; ++index) {
    test_set_online(expected_online[index], 1);
    test_pump(platform);
    CHECK(h2_runtime_poll_event(runtime, &event) == H2_PAL_OK);
    CHECK(event.component == H2_RUNTIME_COMPONENT_SYSTEM_NETIF);
    CHECK(event.kind == H2_RUNTIME_SYSTEM_EVENT_NETIF_DEFAULT_CHANGED);
    CHECK(change->current_valid == (uint8_t)expected_online[index]);
    const h2_runtime_system_netif_ref_t *ref =
        expected_online[index] ? &change->current : &change->previous;
    CHECK(ref->kind == H2_RUNTIME_SYSTEM_NETIF_KIND_HOST);
    CHECK(ref->name_valid == 1u && strcmp(ref->name, "browser") == 0);
    // App-side reconciliation re-reads the default path after the event.
    h2_pal_netif_status_t status;
    const h2_pal_netif_ref_t default_ref = h2_pal_netif_default_ref();
    const h2_pal_result_t status_rc =
        h2_pal_netif_get_status(runtime->netif, &default_ref, &status);
    CHECK(expected_online[index] ? status_rc == H2_PAL_OK &&
                                       h2_pal_netif_status_is_usable(&status)
                                 : status_rc == H2_PAL_ERR_NOT_FOUND);
  }
  CHECK(h2_runtime_poll_event(runtime, &event) != H2_PAL_OK);
  h2_runtime_deinit(runtime);
}

static void test_unsupported_environment(void) {
  test_set_present(0);
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  CHECK(platform != NULL);
  const h2_pal_netif_api_t *netif = h2_web_platform_netif_api(platform);
  const h2_pal_netif_ref_t default_ref = h2_pal_netif_default_ref();
  h2_pal_netif_status_t status;
  h2_pal_netif_ref_t ref;
  const h2_pal_netif_filter_t any = {0};
  int count = 0;
  CHECK(h2_pal_netif_get_status(netif, &default_ref, &status) ==
        H2_PAL_ERR_UNSUPPORTED);
  CHECK(h2_pal_netif_find(netif, &any, &ref) == H2_PAL_ERR_UNSUPPORTED);
  CHECK(h2_pal_netif_list(netif, NULL, test_count_netif, &count) ==
        H2_PAL_ERR_UNSUPPORTED);
  CHECK(test_listener_count() == 0);
  h2_web_platform_destroy(platform);
  test_set_present(1);
}

int main(void) {
  const h2_web_platform_config_t config = {.display_width = 1,
                                           .display_height = 1};
  h2_web_platform_t *platform = h2_web_platform_create(&config);
  CHECK(platform != NULL);
  CHECK(test_listener_count() == 2);
  test_status_contract(platform);
  test_events(platform);
  test_runtime_delivery(platform);
  h2_web_platform_destroy(platform);
  // Destroy removes the browser listeners; late notifications are ignored.
  CHECK(test_listener_count() == 0);
  test_set_online(0, 1);
  test_set_online(1, 1);
  test_unsupported_environment();
  printf("WEB_NETIF PASS\n");
  return 0;
}
