#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/net/h2_pal_netif.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2_jieli_wl82_atomic.h"

enum WIFI_EVENT {
  WIFI_EVENT_STA_START, WIFI_EVENT_STA_SCAN_COMPLETED,
  WIFI_EVENT_STA_CONNECT_SUCC, WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC,
  WIFI_EVENT_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID, WIFI_EVENT_STA_CONNECT_ASSOCIAT_FAIL,
  WIFI_EVENT_STA_CONNECT_ASSOCIAT_TIMEOUT, WIFI_EVENT_STA_NETWORK_STACK_DHCP_TIMEOUT,
  WIFI_EVENT_STA_DISCONNECT, WIFI_EVENT_STA_STOP, WIFI_EVENT_AP_START, WIFI_EVENT_AP_STOP
};
enum { SCAN_IDLE, SCAN_PENDING, SCAN_ABANDONED };
static unsigned scan_phase;
static atomic_int entered, release_payload, readers_ready, reject_got_ip;
static int refresh_error;
static _Thread_local int hold_payload, hold_refresh;
static void assert_sdk_unlocked(void);
#define wifi_is_on() 1
#define os_time_dly(ticks) ((void)(ticks), sched_yield())
static void scan_completed(void) {}
static void scan_reap_completed(void) {}
static int wifi_operation_begin(void);
static void fake_sdk_refresh(void) {
  assert_sdk_unlocked();
  if (hold_refresh) {
    atomic_store(&entered, 1);
    while (!atomic_load(&release_payload)) sched_yield();
  }
}
static void update_sta_snapshot(void) { fake_sdk_refresh(); }
static void wifi_rxfilter_cfg(int value) { (void)value; assert_sdk_unlocked(); }
static void post_system_event(h2_pal_system_event_type_t type, const void *payload, size_t size) {
  assert_sdk_unlocked();
  assert(wifi_operation_begin() == H2_PAL_ERR_BUSY);
  assert(type != H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP || !atomic_load(&reject_got_ip));
  if (hold_payload && type == H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED) {
    assert(size == sizeof(h2_pal_wifi_sta_status_t));
    const h2_pal_wifi_sta_status_t *status = payload;
    assert(status->state == H2_PAL_WIFI_STA_STATE_CONNECTED);
    atomic_store(&entered, 1);
    while (!atomic_load(&release_payload)) sched_yield();
    assert(status->state == H2_PAL_WIFI_STA_STATE_CONNECTED);
  }
}
/* REAL_PROVIDER */
static void assert_sdk_unlocked(void) {
  /* SDK_GATE_CHECK */
}
static void *refresh_held(void *unused) {
  (void)unused;
  hold_refresh = 1;
  assert(wifi_event(NULL, WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC) == 0);
  return NULL;
}
static void *publish_held(void *unused) {
  (void)unused;
  hold_payload = 1;
  assert(wifi_event(NULL, WIFI_EVENT_STA_CONNECT_SUCC) == 0);
  return NULL;
}
static void *publish_many(void *unused) {
  (void)unused;
  while (!atomic_load(&readers_ready)) sched_yield();
  for (int i = 0; i < 10000; ++i) {
    assert(wifi_event(NULL, WIFI_EVENT_STA_CONNECT_SUCC) == 0);
    assert(wifi_event(NULL, WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC) == 0);
    assert(wifi_event(NULL, WIFI_EVENT_STA_DISCONNECT) == 0);
    assert(wifi_event(NULL, WIFI_EVENT_AP_START) == 0);
    assert(wifi_event(NULL, WIFI_EVENT_AP_STOP) == 0);
  }
  return NULL;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  wifi_state.on = 1;
  pthread_t worker;
  if (strcmp(argv[1], "ip_failure") == 0) {
    refresh_error = 1;
    assert(wifi_event(NULL, WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC) == 0);
    h2_pal_wifi_sta_status_t status;
    assert(sta_get_status(NULL, &status) == H2_PAL_OK);
    assert(status.state == H2_PAL_WIFI_STA_STATE_FAILED && !status.ip_valid);
    return 0;
  } else if (strcmp(argv[1], "payload") == 0) {
    assert(pthread_create(&worker, NULL, publish_held, NULL) == 0);
    while (!atomic_load(&entered)) sched_yield();
    assert(wifi_event(NULL, WIFI_EVENT_STA_DISCONNECT) == 0);
    atomic_store(&release_payload, 1);
  } else if (strcmp(argv[1], "stale_refresh") == 0) {
    assert(pthread_create(&worker, NULL, refresh_held, NULL) == 0);
    while (!atomic_load(&entered)) sched_yield();
    assert(wifi_event(NULL, WIFI_EVENT_STA_STOP) == 0);
    atomic_store(&reject_got_ip, 1);
    atomic_store(&release_payload, 1);
  } else {
    assert(strcmp(argv[1], "readers") == 0);
    assert(pthread_create(&worker, NULL, publish_many, NULL) == 0);
    atomic_store(&readers_ready, 1);
    for (int i = 0; i < 10000; ++i) {
      h2_pal_wifi_sta_status_t sta;
      h2_pal_wifi_ap_status_t ap;
      assert(sta_get_status(NULL, &sta) == H2_PAL_OK);
      assert(ap_get_status(NULL, &ap) == H2_PAL_OK);
      assert(sta.state != H2_PAL_WIFI_STA_STATE_GOT_IP || sta.ip_valid);
      assert(sta.state != H2_PAL_WIFI_STA_STATE_DISCONNECTED || !sta.ip_valid);
    }
  }
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
