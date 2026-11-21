#include "h2_esp_wifi_teardown.h"
#include "core/h2_pal_errors.h"
#include "hal/h2_pal_wifi.h"
#include "os/h2_pal_system_event.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef CONFIG_ESP_WIFI_SOFTAP_SUPPORT
#define CONFIG_ESP_WIFI_SOFTAP_SUPPORT 1
#endif

/* As with the AP-stop test, inject SDK calls around extracted production
 * functions rather than maintaining a second implementation of stop(). */
typedef int esp_err_t;
typedef unsigned EventBits_t;
typedef struct test_netif {
  int present;
} esp_netif_t;
typedef struct test_event_group {
  EventBits_t bits;
}* EventGroupHandle_t;
typedef void* esp_timer_handle_t;

enum {
  ESP_OK = 0,
  ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED = 101,
  ESP_ERR_WIFI_NOT_INIT = 102,
  ESP_ERR_WIFI_NOT_STARTED = 103,
  ESP_ERR_WIFI_NOT_CONNECT = 104,
  ESP_ERR_WIFI_NOT_ASSOC = 105,
  TEST_SDK_FAILURE = 106,
  H2_ATOMIC_SEQ_CST = 0,
  H2_ESP_WIFI_EVENT_CONNECTED = 1u << 0,
  H2_ESP_WIFI_EVENT_DISCONNECTED = 1u << 1,
  H2_ESP_WIFI_EVENT_GOT_IP = 1u << 2,
  H2_ESP_WIFI_EVENT_ADDRESS_RELEASED = 1u << 3,
  H2_ESP_WIFI_ADDRESS_EVENT = 1,
  H2_ESP_WIFI_ADDRESS_RELEASE = 1,
  H2_ESP_WIFI_EVENT_TIMEOUT_MS = 1000u,
  pdFALSE = 0,
  pdTRUE = 1,
  TEST_UNRELATED_EVENT = 1u << 8,
};

enum {
  CALL_DISCONNECT = 1,
  CALL_DHCP_STOP,
  CALL_DRIVER_STOP,
  CALL_DRIVER_DEINIT,
  CALL_DESTROY_NETIF,
};

static esp_netif_t s_netif;
static struct test_event_group s_events;
static esp_netif_t* s_h2_esp_wifi_sta_netif;
static EventGroupHandle_t s_h2_esp_wifi_events;
static esp_timer_handle_t s_h2_esp_wifi_address_timer;
static int s_h2_esp_wifi_started;
static int s_h2_esp_wifi_sta_dns_handler_registered;
static int s_h2_esp_wifi_had_ip;
static int s_h2_esp_wifi_sta_disconnect_reason;
static h2_pal_wifi_sta_status_t s_h2_esp_wifi_association;
static int s_h2_esp_wifi_events_registered;
static int s_h2_esp_wifi_address_release_pending;
static int s_h2_esp_wifi_cleanup_pending;
static unsigned s_h2_esp_wifi_connect_busy;
#define pdMS_TO_TICKS(ms) (ms)
static void h2_esp_wifi_release_address_state(void);
static void h2_esp_wifi_post_sta_lost_ip(h2_pal_wifi_sta_state_t next_state,
                                         int disconnect_reason);
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
static int s_h2_esp_wifi_ap_lock;
static int s_h2_esp_wifi_ap_active;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#endif

static struct {
  esp_err_t disconnect_result, dhcp_result, stop_result, deinit_result;
  int reconnect_enabled, reconnect_attempts, activity;
  unsigned reconnect_disabled, timer_stops, netif_destroys, reconciles;
  esp_err_t post_result;
  unsigned release_posts, event_waits, lost_ip_events;
  int release_queued, defer_release, disconnect_first;
  h2_pal_wifi_sta_status_t lost_ip;
  int reenter_stop;
  unsigned calls[16];
  size_t call_count;
} s_sdk;

int h2_esp_platform_wifi_stop(void);

static void record_call(unsigned call) {
  assert(s_sdk.call_count < sizeof(s_sdk.calls) / sizeof(s_sdk.calls[0]));
  s_sdk.calls[s_sdk.call_count++] = call;
}

static int h2_atomic_flag_test_and_set(unsigned* flag, int order) {
  (void)order;
  unsigned previous = *flag;
  *flag = 1u;
  return previous != 0u;
}

static void h2_atomic_flag_clear(unsigned* flag, int order) {
  (void)order;
  *flag = 0u;
}

static void h2_esp_wifi_sta_reconnect_disable(void) {
  assert(s_h2_esp_wifi_connect_busy != 0u);
  s_sdk.reconnect_enabled = 0;
  s_sdk.reconnect_attempts = 0;
  ++s_sdk.reconnect_disabled;
}

static void h2_esp_wifi_set_activity(int active) { s_sdk.activity = active; }

static int h2_esp_wifi_map_error(esp_err_t error) {
  assert(error != ESP_OK);
  return H2_PAL_ERR_IO;
}

static esp_err_t esp_wifi_disconnect(void) {
  assert(s_h2_esp_wifi_connect_busy != 0u);
  assert(!s_sdk.reconnect_enabled);
  record_call(CALL_DISCONNECT);
  if (s_sdk.reenter_stop) {
    s_sdk.reenter_stop = 0;
    assert(h2_esp_platform_wifi_stop() == H2_PAL_ERR_BUSY);
    assert(s_h2_esp_wifi_connect_busy != 0u);
  }
  return s_sdk.disconnect_result;
}

static esp_err_t esp_netif_dhcpc_stop(esp_netif_t* netif) {
  assert(netif == &s_netif);
  record_call(CALL_DHCP_STOP);
  return s_sdk.dhcp_result;
}

static esp_err_t esp_wifi_stop(void) {
  record_call(CALL_DRIVER_STOP);
  return s_sdk.stop_result;
}

static esp_err_t esp_wifi_deinit(void) {
  record_call(CALL_DRIVER_DEINIT);
  return s_sdk.deinit_result;
}

static esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
  assert(timer == &s_netif);
  ++s_sdk.timer_stops;
  return ESP_OK;
}

static void esp_netif_destroy_default_wifi(esp_netif_t* netif) {
  assert(netif == &s_netif);
  assert(s_h2_esp_wifi_sta_netif == NULL);
  record_call(CALL_DESTROY_NETIF);
  ++s_sdk.netif_destroys;
}

static int h2_esp_platform_netif_reconcile_default(void) {
  ++s_sdk.reconciles;
  return H2_PAL_OK;
}

static EventBits_t xEventGroupClearBits(EventGroupHandle_t group,
                                        EventBits_t bits) {
  EventBits_t previous = group->bits;
  group->bits &= ~bits;
  return previous;
}

static EventBits_t xEventGroupSetBits(EventGroupHandle_t group,
                                      EventBits_t bits) {
  group->bits |= bits;
  return group->bits;
}

static void h2_esp_wifi_post_sta_system_event(
    h2_pal_system_event_type_t type, const h2_pal_wifi_sta_status_t* status) {
  assert(type == H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP);
  assert(s_h2_esp_wifi_sta_netif == &s_netif);
  ++s_sdk.lost_ip_events;
  s_sdk.lost_ip = *status;
}

static esp_err_t esp_event_post(int base, int id, const void* data,
                                size_t length, unsigned timeout) {
  assert(base == H2_ESP_WIFI_ADDRESS_EVENT);
  assert(id == H2_ESP_WIFI_ADDRESS_RELEASE && data == NULL && length == 0u);
  assert(timeout == H2_ESP_WIFI_EVENT_TIMEOUT_MS);
  ++s_sdk.release_posts;
  if (s_sdk.post_result == ESP_OK) {
    assert(!s_sdk.release_queued);
    s_sdk.release_queued = 1;
  }
  return s_sdk.post_result;
}

static void deliver_release(void) {
  assert(s_sdk.release_queued);
  if (s_sdk.disconnect_first)
    h2_esp_wifi_post_sta_lost_ip(H2_PAL_WIFI_STA_STATE_DISCONNECTED, 77);
  h2_esp_wifi_release_address_state();
  s_sdk.release_queued = 0;
}

static EventBits_t xEventGroupWaitBits(EventGroupHandle_t group,
                                       EventBits_t bits, int clear, int all,
                                       unsigned timeout) {
  assert(group == &s_events && bits == H2_ESP_WIFI_EVENT_ADDRESS_RELEASED);
  assert(clear == pdFALSE && all == pdTRUE);
  assert(timeout == H2_ESP_WIFI_EVENT_TIMEOUT_MS);
  ++s_sdk.event_waits;
  if (s_sdk.release_queued && !s_sdk.defer_release) deliver_release();
  return group->bits;
}

/* Includes actual address-loss and teardown functions, with SDK/event-loop
 * injection to exercise both disconnect orderings and bounded failure. */
#include "h2_esp_wifi_stop_test_source.inc"

static void reset_connected(void) {
  memset(&s_sdk, 0, sizeof(s_sdk));
  s_h2_esp_wifi_sta_netif = &s_netif;
  s_h2_esp_wifi_events = &s_events;
  s_h2_esp_wifi_address_timer = &s_netif;
  s_events.bits = H2_ESP_WIFI_EVENT_CONNECTED | H2_ESP_WIFI_EVENT_GOT_IP |
                  TEST_UNRELATED_EVENT;
  s_h2_esp_wifi_started = 1;
  s_h2_esp_wifi_sta_dns_handler_registered = 1;
  s_h2_esp_wifi_events_registered = 1;
  s_h2_esp_wifi_address_release_pending = 0;
  s_h2_esp_wifi_cleanup_pending = 0;
  s_h2_esp_wifi_had_ip = 1;
  s_h2_esp_wifi_sta_disconnect_reason = 77;
  memset(&s_h2_esp_wifi_association, 0, sizeof(s_h2_esp_wifi_association));
  s_h2_esp_wifi_association.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  s_h2_esp_wifi_association.ip_valid = 1u;
  memset(&s_h2_esp_wifi_association.ip, 0x5a,
         sizeof(s_h2_esp_wifi_association.ip));
  s_h2_esp_wifi_association.ssid_len = 4u;
  memcpy(s_h2_esp_wifi_association.ssid, "wifi", 5u);
  s_h2_esp_wifi_connect_busy = 0u;
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
  s_h2_esp_wifi_ap_active = 0;
#endif
  s_sdk.reconnect_enabled = 1;
  s_sdk.reconnect_attempts = 2;
}

static void assert_released(void) {
  const unsigned char empty[sizeof(s_h2_esp_wifi_association)] = {0};
  assert(s_h2_esp_wifi_sta_netif == NULL);
  assert(s_h2_esp_wifi_started == 0);
  assert(s_h2_esp_wifi_sta_dns_handler_registered == 0);
  assert(s_h2_esp_wifi_had_ip == 0);
  assert(s_h2_esp_wifi_sta_disconnect_reason == 0);
  assert(memcmp(&s_h2_esp_wifi_association, &empty, sizeof(empty)) == 0);
  assert(s_events.bits ==
         (H2_ESP_WIFI_EVENT_DISCONNECTED | H2_ESP_WIFI_EVENT_ADDRESS_RELEASED |
          TEST_UNRELATED_EVENT));
  assert(s_h2_esp_wifi_address_release_pending == 0);
  assert(s_h2_esp_wifi_cleanup_pending == 0);
  assert(s_sdk.reconnect_enabled == 0);
  assert(s_sdk.reconnect_attempts == 0);
  assert(s_sdk.activity == 0);
  assert(s_h2_esp_wifi_connect_busy == 0u);
}

static void test_successful_release(void) {
  reset_connected();
  s_sdk.reenter_stop = 1;
  assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
  assert_released();
  assert(s_sdk.call_count == 5u);
  for (unsigned i = 0; i < 5u; ++i) assert(s_sdk.calls[i] == i + 1u);
  assert(s_sdk.timer_stops == 1u);
  assert(s_sdk.netif_destroys == 1u);
  assert(s_sdk.reconciles == 1u);
  assert(s_sdk.release_posts == 1u && s_sdk.event_waits == 1u);
  assert(s_sdk.lost_ip_events == 1u);
  assert(s_sdk.lost_ip.state == H2_PAL_WIFI_STA_STATE_DISCONNECTED);
  assert(s_sdk.lost_ip.disconnect_reason == 77);
  assert(s_sdk.lost_ip.ssid_len == 4u);
  assert(memcmp(s_sdk.lost_ip.ssid, "wifi", 5u) == 0);
  assert(s_sdk.lost_ip.ip_valid == 0u);
  const unsigned char empty_ip[sizeof(s_sdk.lost_ip.ip)] = {0};
  assert(memcmp(&s_sdk.lost_ip.ip, empty_ip, sizeof(empty_ip)) == 0);
  /* A disconnect delivered after release must not publish another LOST_IP. */
  h2_esp_wifi_post_sta_lost_ip(H2_PAL_WIFI_STA_STATE_DISCONNECTED, 77);
  assert(s_sdk.lost_ip_events == 1u);
}

static void test_busy_transaction_is_untouched(void) {
  reset_connected();
  s_h2_esp_wifi_connect_busy = 1u;
  assert(h2_esp_platform_wifi_stop() == H2_PAL_ERR_BUSY);
  assert(s_h2_esp_wifi_connect_busy == 1u);
  assert(s_sdk.call_count == 0u);
  assert(s_sdk.reconnect_disabled == 0u);
  assert(s_h2_esp_wifi_started == 1);
  assert(s_h2_esp_wifi_sta_netif == &s_netif);
}

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
static void test_active_ap_is_untouched(void) {
  reset_connected();
  s_h2_esp_wifi_ap_active = 1;
  assert(h2_esp_platform_wifi_stop() == H2_PAL_ERR_BUSY);
  assert(s_h2_esp_wifi_connect_busy == 0u);
  assert(s_h2_esp_wifi_ap_active == 1);
  assert(s_sdk.call_count == 0u);
  assert(s_sdk.reconnect_disabled == 0u);
  assert(s_h2_esp_wifi_started == 1);
  assert(s_h2_esp_wifi_sta_netif == &s_netif);
}
#endif

static void test_repeated_stop(void) {
  reset_connected();
  assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
  s_sdk.disconnect_result = ESP_ERR_WIFI_NOT_INIT;
  s_sdk.stop_result = ESP_ERR_WIFI_NOT_INIT;
  s_sdk.deinit_result = ESP_ERR_WIFI_NOT_INIT;
  assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
  assert_released();
  assert(s_sdk.netif_destroys == 1u);
  assert(s_sdk.reconciles == 1u);
}

static void test_partial_start_is_released(void) {
  reset_connected();
  s_h2_esp_wifi_started = 0;
  s_sdk.disconnect_result = ESP_ERR_WIFI_NOT_STARTED;
  s_sdk.stop_result = ESP_ERR_WIFI_NOT_STARTED;
  assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
  assert_released();
  assert(s_sdk.netif_destroys == 1u);
}

static void test_cold_stop_without_shared_objects(void) {
  reset_connected();
  s_h2_esp_wifi_started = 0;
  s_h2_esp_wifi_sta_netif = NULL;
  s_h2_esp_wifi_events = NULL;
  s_h2_esp_wifi_address_timer = NULL;
  s_sdk.disconnect_result = ESP_ERR_WIFI_NOT_INIT;
  s_sdk.stop_result = ESP_ERR_WIFI_NOT_INIT;
  s_sdk.deinit_result = ESP_ERR_WIFI_NOT_INIT;
  assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
  assert(s_sdk.netif_destroys == 0u);
  assert(s_sdk.timer_stops == 0u);
  assert(s_h2_esp_wifi_connect_busy == 0u);
  assert(s_sdk.activity == 0);
}

static void test_failure_retains_ownership_and_can_retry(void) {
  for (unsigned stage = CALL_DISCONNECT; stage <= CALL_DRIVER_DEINIT; ++stage) {
    reset_connected();
    esp_err_t* result = stage == CALL_DISCONNECT    ? &s_sdk.disconnect_result
                        : stage == CALL_DHCP_STOP   ? &s_sdk.dhcp_result
                        : stage == CALL_DRIVER_STOP ? &s_sdk.stop_result
                                                    : &s_sdk.deinit_result;
    *result = TEST_SDK_FAILURE;
    assert(h2_esp_platform_wifi_stop() == H2_PAL_ERR_IO);
    assert(s_sdk.call_count == stage);
    assert(s_h2_esp_wifi_sta_netif == &s_netif);
    assert(s_h2_esp_wifi_started == 1);
    assert(s_h2_esp_wifi_sta_dns_handler_registered == 1);
    assert(s_sdk.netif_destroys == 0u);
    assert(s_h2_esp_wifi_connect_busy == 0u);
    assert(s_sdk.activity == 0);
    assert(s_sdk.reconnect_enabled == 0);
    *result = ESP_OK;
    if (stage == CALL_DRIVER_DEINIT) {
      /* Stop already succeeded; the retry must accept an inactive driver. */
      s_sdk.disconnect_result = ESP_ERR_WIFI_NOT_STARTED;
      s_sdk.stop_result = ESP_ERR_WIFI_NOT_STARTED;
      s_sdk.dhcp_result = ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED;
    }
    s_sdk.call_count = 0u;
    assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
    assert_released();
    assert(s_sdk.netif_destroys == 1u);
  }
}

static void test_unassociated_station_can_stop(void) {
  const esp_err_t reasons[] = {ESP_ERR_WIFI_NOT_CONNECT,
                               ESP_ERR_WIFI_NOT_ASSOC};
  for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
    reset_connected();
    s_sdk.disconnect_result = reasons[i];
    assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
    assert_released();
  }
}

static void test_disconnect_before_release_publishes_once(void) {
  reset_connected();
  s_sdk.disconnect_first = 1;
  assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
  assert_released();
  assert(s_sdk.lost_ip_events == 1u);
  assert(s_sdk.lost_ip.ssid_len == 4u && s_sdk.lost_ip.ip_valid == 0u);
}

static void test_event_post_failure_retains_address_state(void) {
  reset_connected();
  s_sdk.post_result = TEST_SDK_FAILURE;
  assert(h2_esp_platform_wifi_stop() == H2_PAL_ERR_IO);
  assert(s_h2_esp_wifi_sta_netif == &s_netif);
  assert(s_h2_esp_wifi_had_ip == 1);
  assert(s_h2_esp_wifi_association.ssid_len == 4u);
  assert(s_h2_esp_wifi_address_release_pending == 0);
  assert(s_h2_esp_wifi_cleanup_pending == 1);
  assert(s_sdk.lost_ip_events == 0u && s_sdk.netif_destroys == 0u);
  assert(s_sdk.event_waits == 0u && s_h2_esp_wifi_connect_busy == 0u);
  s_sdk.post_result = ESP_OK;
  assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
  assert_released();
  assert(s_sdk.release_posts == 2u && s_sdk.lost_ip_events == 1u);
}

static void test_event_timeout_retry_reuses_pending_release(void) {
  for (int late = 0; late <= 1; ++late) {
    reset_connected();
    s_sdk.defer_release = 1;
    assert(h2_esp_platform_wifi_stop() == H2_PAL_ERR_TIMEOUT);
    assert(s_h2_esp_wifi_sta_netif == &s_netif);
    assert(s_h2_esp_wifi_had_ip == 1);
    assert(s_h2_esp_wifi_association.ssid_len == 4u);
    assert(s_h2_esp_wifi_address_release_pending == 1);
    assert(s_h2_esp_wifi_cleanup_pending == 1);
    assert(s_sdk.netif_destroys == 0u && s_sdk.lost_ip_events == 0u);
    assert(s_h2_esp_wifi_connect_busy == 0u);
    assert(h2_esp_platform_wifi_stop() == H2_PAL_ERR_TIMEOUT);
    assert(s_sdk.release_posts == 1u && s_sdk.event_waits == 2u);
    if (late) deliver_release();
    s_sdk.defer_release = 0;
    assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
    assert_released();
    assert(s_sdk.release_posts == 1u && s_sdk.event_waits == 3u);
    assert(s_sdk.lost_ip_events == 1u && s_sdk.netif_destroys == 1u);
  }
}

static void test_stop_without_address_does_not_publish_loss(void) {
  reset_connected();
  s_h2_esp_wifi_had_ip = 0;
  assert(h2_esp_platform_wifi_stop() == H2_PAL_OK);
  assert_released();
  assert(s_sdk.lost_ip_events == 0u);
}

int main(void) {
  test_successful_release();
  test_disconnect_before_release_publishes_once();
  test_event_post_failure_retains_address_state();
  test_event_timeout_retry_reuses_pending_release();
  test_stop_without_address_does_not_publish_loss();
  test_busy_transaction_is_untouched();
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
  test_active_ap_is_untouched();
#endif
  test_repeated_stop();
  test_partial_start_is_released();
  test_cold_stop_without_shared_objects();
  test_failure_retains_ownership_and_can_retry();
  test_unassociated_station_can_stop();
  return 0;
}
