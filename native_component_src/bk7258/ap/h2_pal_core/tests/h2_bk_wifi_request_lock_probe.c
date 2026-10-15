#define _POSIX_C_SOURCE 200809L
#include "h2/pal/hal/h2_pal_wifi_settings.h"
#include "h2/pal/net/h2_pal_netif.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2_bk_wifi_ipv6_internal.h"
#include "h2_bk_wifi_rpc.h"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LWIP_IPV6 1
#define BK_LOGI(...) ((void)0)
typedef int bk_err_t;
typedef int wifi_link_state_t;
typedef pthread_mutex_t *beken_mutex_t;
enum {
  BK_OK = 0, BK_FAIL = -1, BK_ERR_WIFI_STA_NOT_STARTED = -2,
  BK_ERR_WIFI_STA_NOT_CONFIG = -3, BK_ERR_WIFI_DRIVER = -4, kNoErr = 0,
  BEKEN_APPLICATION_PRIORITY = 1, WIFI_SECURITY_AUTO = 0,
  NETIF_IF_STA = 0, AP_GET_NETIF_IP4_CONFIG = 999,
  WIFI_LINKSTATE_STA_IDLE = 0, WIFI_LINKSTATE_STA_CONNECTING,
  WIFI_LINKSTATE_STA_CONNECTED, WIFI_LINKSTATE_STA_GOT_IP,
  WIFI_LINKSTATE_STA_DISCONNECTED, WIFI_LINKSTATE_STA_CONNECT_FAILED,
  WIFI_LINKSTATE_STA_SCAN_DONE
};
typedef struct {
  char ssid[33], password[65];
  uint8_t bssid[6], channel;
  int security, is_not_support_auto_fci;
  int auto_reconnect_count, auto_reconnect_timeout;
} wifi_sta_config_t;
typedef struct {
  int state;
  char ssid[33];
  uint8_t bssid[6], channel;
  int rssi;
} wifi_link_status_t;
typedef struct { char ip[16], mask[16], gateway[16]; } netif_ip4_config_t;
typedef struct {
  h2_pal_wifi_sta_config_t requested_config;
  wifi_sta_config_t sdk_config;
  uint32_t generation;
} h2_bk_wifi_connect_request_t;
typedef int err_t;
enum { ERR_OK = 0, ERR_IF = -1 };
struct tcpip_api_call_data { int unused; };
struct netif { int unused; };

static pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ipv6_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
static beken_mutex_t s_h2_bk_wifi_request_mutex = &request_mutex;
static beken_mutex_t s_h2_bk_wifi_ipv6_mutex = &ipv6_mutex;
static beken_mutex_t s_h2_bk_wifi_status_mutex = &status_mutex;
static beken_mutex_t s_h2_bk_wifi_event_mutex = &event_mutex;
static int s_h2_bk_wifi_power_save_error, s_h2_bk_wifi_connect_pending;
static int s_h2_bk_wifi_last_config_valid, s_h2_bk_wifi_sta_status_valid;
static uint32_t s_h2_bk_wifi_connect_generation, s_h2_bk_wifi_cp_generation;
static uint32_t s_h2_bk_wifi_had_ip;
static h2_pal_wifi_sta_config_t s_h2_bk_wifi_last_config;
static h2_pal_wifi_sta_status_t s_h2_bk_wifi_connect_status;
static h2_pal_wifi_sta_status_t s_h2_bk_wifi_sta_status;

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int interleave, worker_has_ipv6, connector_has_request;
static int worker_waiting_request, completed;
static _Thread_local int refresh_worker;
static unsigned snapshots, reconfigured;
static void *allocation;
static struct netif station;
static const uint8_t preferred[16] = {0xfd, 0x42, [15] = 42};

static void wait_value(int *value, int expected) {
  struct timespec deadline;
  assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 5;
  assert(pthread_mutex_lock(&gate) == 0);
  while (*value < expected) {
    if (pthread_cond_timedwait(&changed, &gate, &deadline) != 0) {
      fprintf(stderr, "repeat-connect/IPv6 worker completion timed out\n");
      abort();
    }
  }
  assert(pthread_mutex_unlock(&gate) == 0);
}

static int rtos_lock_mutex(beken_mutex_t *mutex) {
  if (interleave && refresh_worker && *mutex == &request_mutex) {
    assert(pthread_mutex_lock(&gate) == 0);
    while (!connector_has_request)
      assert(pthread_cond_wait(&changed, &gate) == 0);
    worker_waiting_request = 1;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
  }
  assert(pthread_mutex_lock(*mutex) == 0);
  if (interleave && refresh_worker && *mutex == &ipv6_mutex) {
    assert(pthread_mutex_lock(&gate) == 0);
    worker_has_ipv6 = 1;
    assert(pthread_cond_broadcast(&changed) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
  }
  if (interleave && !refresh_worker && *mutex == &request_mutex) {
    assert(pthread_mutex_lock(&gate) == 0);
    connector_has_request = 1;
    assert(pthread_cond_broadcast(&changed) == 0);
    while (!worker_waiting_request)
      assert(pthread_cond_wait(&changed, &gate) == 0);
    assert(pthread_mutex_unlock(&gate) == 0);
  }
  return kNoErr;
}
static int rtos_unlock_mutex(beken_mutex_t *mutex) {
  assert(pthread_mutex_unlock(*mutex) == 0);
  return kNoErr;
}
static void *os_malloc(size_t size) { return allocation = malloc(size); }
static void os_free(void *pointer) { free(pointer); allocation = NULL; }
static int h2_bk_wifi_map_error(int error) { (void)error; return H2_PAL_ERR_IO; }
static int h2_bk_wifi_ensure_events_registered(void) { return H2_PAL_OK; }
static int h2_bk_wifi_ap_active(void) { return 0; }
static int h2_bk_wifi_parse_ip4(const char *text, uint32_t *out) {
  (void)text; *out = 0u; return 0;
}
static int bk_wifi_sta_get_link_status(wifi_link_status_t *out) {
  *out = (wifi_link_status_t){.state = WIFI_LINKSTATE_STA_CONNECTED};
  memcpy(out->ssid, "repeat", 7u);
  return BK_OK;
}
static int bk_netif_get_ip4_config(int interface, netif_ip4_config_t *out) {
  (void)interface; (void)out; return BK_FAIL;
}
static int netif_status(void *user, const h2_pal_netif_ref_t *ref,
                        h2_pal_netif_status_t *out) {
  (void)user; (void)ref;
  *out = (h2_pal_netif_status_t){.kind = H2_PAL_NETIF_KIND_WIFI_STA,
      .flags = H2_PAL_NETIF_FLAG_HAS_IPV6};
  out->ipv6.family = H2_PAL_NET_FAMILY_IPV6;
  memcpy(out->ipv6.ip, preferred, sizeof(preferred));
  return H2_PAL_OK;
}
static const h2_pal_netif_api_t *h2_bk_platform_netif_api(void) {
  static const h2_pal_netif_vtable_t vtable = {.get_status = netif_status};
  static const h2_pal_netif_api_t api = {.vtable = &vtable};
  return &api;
}
static int h2_bk_platform_netif_reconcile_default_async(void) { return 0; }
static void h2_bk_wifi_post_sta_system_event(h2_pal_system_event_type_t type,
                                           const h2_pal_wifi_sta_status_t *status) {
  (void)type; (void)status;
}
static void *net_get_sta_handle(void) { return &station; }
static int h2_bk_wifi_ipv6_install(struct netif *netif,
    const h2_bk_wifi_ipv6_snapshot_t *snapshot, h2_pal_wifi_sta_status_t *out,
    uint32_t *generation) {
  assert(netif == &station);
  memcpy(out->ip.ip6, snapshot->addresses[0].address, 16u);
  out->ip.ip6_valid = 1u;
  *generation = snapshot->generation;
  return ERR_OK;
}
static int tcpip_api_call(int (*fn)(struct tcpip_api_call_data *),
                          struct tcpip_api_call_data *data) { return fn(data); }
static int wifi_send_com_api_cmd(unsigned command, unsigned argc, ...) {
  if (command != H2_BK_WIFI_RPC_IPV6_SNAPSHOT) {
    ++reconfigured; return BK_FAIL;
  }
  assert(argc == 1u && allocation != NULL);
  h2_bk_wifi_ipv6_snapshot_t *out = allocation;
  assert(out->size == sizeof(*out));
  *out = (h2_bk_wifi_ipv6_snapshot_t){.version = H2_BK_WIFI_IPV6_VERSION,
      .size = sizeof(*out), .generation = 1u, .connected = 1u, .count = 1u,
      .ssid_len = 6u};
  memcpy(out->ssid, "repeat", 6u);
  out->addresses[0].preferred = 1u;
  memcpy(out->addresses[0].address, preferred, 16u);
  ++snapshots;
  return BK_OK;
}
static void h2_bk_wifi_connect_worker(void *user) { (void)user; abort(); }
static int rtos_create_thread(void *handle, int priority, const char *name,
    void (*fn)(void *), unsigned stack, void *user) {
  (void)handle; (void)priority; (void)name; (void)fn; (void)stack; (void)user;
  ++reconfigured; return -1;
}
static int wifi_sta_is_started(void) { return 1; }
static int bk_wifi_sta_set_config(const wifi_sta_config_t *config) {
  (void)config; ++reconfigured; return BK_FAIL;
}
static int bk_wifi_sta_start(void) { ++reconfigured; return BK_FAIL; }
static int bk_wifi_sta_pm_disable(void) { ++reconfigured; return BK_FAIL; }
static int bk_wifi_sta_connect(void) { ++reconfigured; return BK_FAIL; }
static void rtos_delay_milliseconds(unsigned delay) { (void)delay; abort(); }
static int h2_bk_wifi_ipv6_refresh(void);

/* PRODUCTION_FUNCTIONS */

static void finish(void) {
  assert(pthread_mutex_lock(&gate) == 0);
  ++completed;
  assert(pthread_cond_broadcast(&changed) == 0);
  assert(pthread_mutex_unlock(&gate) == 0);
}
static void *refresh(void *user) {
  (void)user;
  refresh_worker = 1;
  assert(h2_bk_wifi_ipv6_refresh() == H2_PAL_OK);
  finish();
  return NULL;
}
static void *connect_again(void *user) {
  const uint32_t timeout = *(const uint32_t *)user;
  assert(h2_bk_wifi_sta_connect(NULL, &s_h2_bk_wifi_last_config, timeout) == H2_PAL_OK);
  finish();
  return NULL;
}
static void repeat_case(int cached, uint32_t timeout) {
  s_h2_bk_wifi_last_config = (h2_pal_wifi_sta_config_t){.ssid_len = 6u};
  memcpy(s_h2_bk_wifi_last_config.ssid, "repeat", 7u);
  const h2_pal_wifi_sta_config_t original = s_h2_bk_wifi_last_config;
  s_h2_bk_wifi_last_config_valid = 1;
  s_h2_bk_wifi_connect_generation = 9u;
  s_h2_bk_wifi_cp_generation = 0u;
  s_h2_bk_wifi_sta_status_valid = cached;
  s_h2_bk_wifi_sta_status = (h2_pal_wifi_sta_status_t){
      .state = H2_PAL_WIFI_STA_STATE_GOT_IP, .ssid_len = 6u};
  memcpy(s_h2_bk_wifi_sta_status.ssid, "repeat", 7u);
  s_h2_bk_wifi_sta_status.ip.ip6_valid = 1u;
  memcpy(s_h2_bk_wifi_sta_status.ip.ip6, preferred, 16u);
  worker_has_ipv6 = connector_has_request = worker_waiting_request = completed = 0;
  snapshots = reconfigured = 0u;
  interleave = 1;
  pthread_t worker, connector;
  assert(pthread_create(&worker, NULL, refresh, NULL) == 0);
  wait_value(&worker_has_ipv6, 1);
  assert(pthread_create(&connector, NULL, connect_again, &timeout) == 0);
  wait_value(&completed, 2);
  assert(pthread_join(connector, NULL) == 0 && pthread_join(worker, NULL) == 0);
  interleave = 0;
  assert(snapshots == 1u && reconfigured == 0u);
  assert(s_h2_bk_wifi_connect_generation == 9u && s_h2_bk_wifi_cp_generation == 1u);
  assert(memcmp(&original, &s_h2_bk_wifi_last_config, sizeof(original)) == 0);
  h2_pal_wifi_sta_status_t status;
  assert(h2_bk_wifi_sta_get_status(NULL, &status) == H2_PAL_OK);
  assert(snapshots == 2u && h2_pal_wifi_sta_status_has_ip(&status));
  assert(status.ip.ip6_valid && !status.ip_valid);
  assert(memcmp(status.ip.ip6, preferred, 16u) == 0);
  assert(h2_bk_wifi_sta_get_status(NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(snapshots == 2u);
}
int main(void) {
  repeat_case(1, 0u);
  repeat_case(1, 100u);
  repeat_case(0, 0u);
  repeat_case(0, 100u);
  puts("BK repeat connect: cached/SDK status, async/blocking and active IPv6 worker PASS");
  return 0;
}
