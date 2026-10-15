#include "h2_bk_wifi_ipv6_sync.h"
#include "lwip/priv/tcpip_priv.h"
#include "modules/wifi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

const ip6_addr_t test_ip6_zero;
static struct netif station;
struct netif *netif_default;
static struct netif *current = &station;
static wifi_link_status_t link;
static test_event_callback_t connected_callback, disconnected_callback;
static int link_error, tcpip_error, race, in_tcpip;
static unsigned cleanup_count, api_calls;
static int capture_race;
static int capture_disconnect, publish_disconnect;
static int observe_no_ready;
static void (*cache_tick)(void *);

void test_after_atomic_store(void) {
  if (publish_disconnect) {
    publish_disconnect = 0;
    disconnected_callback(NULL, EVENT_MOD_WIFI, EVENT_WIFI_STA_DISCONNECTED,
                          NULL);
    observe_no_ready = 1;
  }
  if (observe_no_ready)
    assert(!h2_bk_wifi_ipv6_ready());
}

void test_capture_edge(void) {
  if (capture_disconnect) {
    capture_disconnect = 0;
    disconnected_callback(NULL, EVENT_MOD_WIFI, EVENT_WIFI_STA_DISCONNECTED,
                          NULL);
    observe_no_ready = 1;
  }
  if (capture_race) {
    capture_race = 0;
    connected_callback(NULL, EVENT_MOD_WIFI, EVENT_WIFI_STA_CONNECTED, NULL);
  }
}
void *net_get_sta_handle(void) { return current; }
int bk_wifi_sta_get_link_status(wifi_link_status_t *out) {
  *out = link;
  if (race)
    connected_callback(NULL, EVENT_MOD_WIFI, EVENT_WIFI_STA_CONNECTED, NULL);
  return link_error;
}
int bk_event_register_cb(event_module_t module, int event,
                         test_event_callback_t fn, void *user) {
  (void)module;
  (void)user;
  if (event == EVENT_WIFI_STA_CONNECTED)
    connected_callback = fn;
  else
    disconnected_callback = fn;
  return BK_OK;
}
err_t tcpip_api_call(err_t (*fn)(struct tcpip_api_call_data *),
                     struct tcpip_api_call_data *data) {
  ++api_calls;
  if (tcpip_error)
    return ERR_IF;
  in_tcpip = 1;
  err_t rc = fn(data);
  in_tcpip = 0;
  if (race)
    connected_callback(NULL, EVENT_MOD_WIFI, EVENT_WIFI_STA_CONNECTED, NULL);
  return rc;
}
void netif_ip6_addr_set_state(struct netif *n, int i, unsigned s) {
  assert(in_tcpip);
  n->state[i] = s;
}
void netif_ip6_addr_set(struct netif *n, int i, const ip6_addr_t *a) {
  assert(in_tcpip);
  n->ip6[i] = *a;
}
void netif_set_link_down(struct netif *n) {
  assert(in_tcpip);
  n->link = 0;
}
void netif_set_link_up(struct netif *n) {
  assert(in_tcpip);
  n->link = 1;
}
void netif_set_down(struct netif *n) {
  assert(in_tcpip);
  n->up = 0;
}
void netif_set_up(struct netif *n) {
  assert(in_tcpip);
  n->up = 1;
}
void netif_set_addr(struct netif *n, const ip4_addr_t *ip,
                    const ip4_addr_t *mask, const ip4_addr_t *gw) {
  assert(in_tcpip);
  n->ip4 = *ip;
  n->mask = *mask;
  n->gw = *gw;
}
void netif_set_default(struct netif *n) {
  assert(in_tcpip);
  netif_default = n;
}
void nd6_cleanup_netif(struct netif *n) {
  (void)n;
  assert(in_tcpip);
  ++cleanup_count;
}
err_t tcpip_callback(void (*fn)(void *), void *user) {
  if (tcpip_error)
    return ERR_IF;
  in_tcpip = 1;
  fn(user);
  in_tcpip = 0;
  return ERR_OK;
}
void sys_timeout(uint32_t delay, void (*fn)(void *), void *user) {
  (void)user;
  assert(in_tcpip && delay == 250u);
  cache_tick = fn;
}
static void tick(void) {
  if (cache_tick) {
    in_tcpip = 1;
    cache_tick(NULL);
    in_tcpip = 0;
  }
}
static h2_bk_wifi_ipv6_snapshot_t snapshot(void) {
  tick();
  h2_bk_wifi_ipv6_snapshot_t out = {.size = sizeof(out)};
  assert(h2_bk_wifi_ipv6_snapshot(&out) == BK_OK);
  return out;
}
int main(int argc, char **argv) {
  unsigned selected = 3u;
  if (argc == 2) {
    if (!strcmp(argv[1], "after-disconnect"))
      selected = 0u;
    else if (!strcmp(argv[1], "during-capture"))
      selected = 1u;
    else if (!strcmp(argv[1], "during-publication"))
      selected = 2u;
    else
      assert(!"unknown CP cache interleaving");
  } else {
    assert(argc == 1);
  }
  station = (struct netif){.up = 1, .link = 1, .index = 2, .autoconfig = 1};
  link = (wifi_link_status_t){.state = WIFI_LINKSTATE_STA_GOT_IP,
                              .ssid = "bench",
                              .bssid = {2, 3, 4, 5, 6, 7},
                              .channel = 6,
                              .rssi = -40};
  const uint8_t ll[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
  const uint8_t ula[16] = {0xfd, 0x53, 0, 0, 0, 0, 0, 0,
                           0,    0,    0, 0, 0, 0, 0, 2};
  memcpy(station.ip6[0].addr, ll, 16);
  station.state[0] = IP6_ADDR_PREFERRED;
  h2_bk_wifi_ipv6_snapshot_t out = snapshot();
  assert(out.count == 1 && !h2_bk_wifi_ipv6_ready());
  memcpy(station.ip6[1].addr, ula, 16);
  station.state[1] = IP6_ADDR_PREFERRED;
  out = snapshot();
  assert(out.count == 2 && h2_bk_wifi_ipv6_ready());
  for (unsigned boundary = 0u; boundary < 3u; ++boundary) {
    if (selected != 3u && selected != boundary)
      continue;
    uint32_t previous_epoch = out.generation;
    if (boundary == 0u) {
      /* Radio disconnect precedes lwIP teardown and even a stale SDK status
       * reply. A refresh must not re-arm ready under the new epoch. */
      disconnected_callback(NULL, EVENT_MOD_WIFI, EVENT_WIFI_STA_DISCONNECTED,
                            NULL);
      observe_no_ready = 1;
    } else if (boundary == 1u) {
      /* Inject the real event while production reads an old preferred address
       * after capturing generation. Observe every later atomic store. */
      capture_disconnect = 1;
    } else {
      /* Also inject after validation, at the first cache publication store. */
      publish_disconnect = 1;
    }
    tick();
    assert(!capture_disconnect && !publish_disconnect &&
           !h2_bk_wifi_ipv6_ready());
    if (boundary != 0u) {
      h2_bk_wifi_ipv6_snapshot_t stale = {.size = sizeof(stale)};
      int stale_rc = h2_bk_wifi_ipv6_snapshot(&stale);
      assert((stale_rc == BK_ERR_BUSY && stale.version == 0u) ||
             (stale_rc == BK_OK && stale.generation != previous_epoch &&
              !stale.connected && stale.count == 0u));
    }
    tick();
    observe_no_ready = 0;
    out = snapshot();
    assert(station.up && station.state[1] == IP6_ADDR_PREFERRED);
    assert(link.state == WIFI_LINKSTATE_STA_GOT_IP);
    assert(out.generation != previous_epoch && !out.connected &&
           out.count == 0u &&
           !h2_bk_wifi_ipv6_ready());
    connected_callback(NULL, EVENT_MOD_WIFI, EVENT_WIFI_STA_CONNECTED, NULL);
    out = snapshot();
    assert(out.count == 2 && h2_bk_wifi_ipv6_ready());
  }
  if (selected != 3u) {
    puts("BK CP production controlled disconnect/cache interleaving PASS");
    return 0;
  }
  h2_pal_wifi_sta_config_t config = {.ssid = "bench", .ssid_len = 5};
  assert(h2_bk_wifi_ipv6_snapshot_matches(&out, &config));
  uint32_t generation = 0;
  h2_pal_wifi_sta_status_t status = {0};
  station.state[1] = IP6_ADDR_INVALID;
  in_tcpip = 1;
  assert(h2_bk_wifi_ipv6_install(&station, &out, &status, &generation) ==
         ERR_OK);
  in_tcpip = 0;
  assert(status.ip.ip6_valid && !status.ip_valid &&
         !memcmp(status.ip.ip6, ula, 16));
  assert(station.ip6[0].zone == 2 && station.ip6[1].zone == 0 &&
         !station.autoconfig);
  assert(netif_default == &station && cleanup_count == 1);
  in_tcpip = 1;
  assert(h2_bk_wifi_ipv6_install(&station, &out, &status, &generation) ==
         ERR_OK);
  in_tcpip = 0;
  assert(cleanup_count == 1);
  /* A new generation with three non-link-local addresses cannot fit while
   * reserving AP slot zero for link-local. Reject before changing anything. */
  h2_bk_wifi_ipv6_snapshot_t oversized = out;
  ++oversized.generation;
  oversized.count = 3u;
  for (uint32_t i = 0u; i < oversized.count; ++i) {
    memcpy(oversized.addresses[i].address, ula, sizeof(ula));
    oversized.addresses[i].address[15] = (uint8_t)(i + 3u);
  }
  uint8_t previous_station[sizeof(station)];
  uint8_t previous_status[sizeof(status)];
  memcpy(previous_station, &station, sizeof(station));
  memcpy(previous_status, &status, sizeof(status));
  uint32_t previous_generation = generation;
  in_tcpip = 1;
  assert(h2_bk_wifi_ipv6_install(&station, &oversized, &status, &generation) ==
         ERR_BUF);
  in_tcpip = 0;
  assert(memcmp(previous_station, &station, sizeof(station)) == 0);
  assert(memcmp(previous_status, &status, sizeof(status)) == 0);
  assert(generation == previous_generation && cleanup_count == 1u);
  out.size--;
  assert(!h2_bk_wifi_ipv6_snapshot_matches(&out, &config));
  out.size++;
  out.version++;
  assert(!h2_bk_wifi_ipv6_snapshot_matches(&out, &config));
  out.version--;
  out.count = 4;
  assert(!h2_bk_wifi_ipv6_snapshot_matches(&out, &config));
  out.count = 2;
  out.ssid[0] = 'x';
  assert(!h2_bk_wifi_ipv6_snapshot_matches(&out, &config));
  out.ssid[0] = 'b';
  config.bssid_set = 1;
  assert(!h2_bk_wifi_ipv6_snapshot_matches(&out, &config));
  memcpy(config.bssid, link.bssid, 6);
  assert(h2_bk_wifi_ipv6_snapshot_matches(&out, &config));
  race = 1;
  out = (h2_bk_wifi_ipv6_snapshot_t){.size = sizeof(out)};
  assert(h2_bk_wifi_ipv6_snapshot(&out) == BK_ERR_BUSY && out.version == 0 &&
         !h2_bk_wifi_ipv6_ready());
  race = 0;
  out = snapshot();
  assert(h2_bk_wifi_ipv6_ready());
  capture_race = 1;
  tick();
  assert(!h2_bk_wifi_ipv6_ready());
  /* The stale writer published ready=1 for the previous epoch. The next
   * empty current-epoch publication must never combine it with a new epoch. */
  current = NULL;
  observe_no_ready = 1;
  tick();
  observe_no_ready = 0;
  assert(!h2_bk_wifi_ipv6_ready());
  current = &station;
  out = snapshot();
  assert(h2_bk_wifi_ipv6_ready());
  current = NULL;
  out = snapshot();
  assert(out.count == 0 && !h2_bk_wifi_ipv6_ready());
  current = &station;
  station.up = 0;
  out = snapshot();
  assert(out.count == 0 && !h2_bk_wifi_ipv6_ready());
  station.up = 1;
  out = snapshot();
  assert(h2_bk_wifi_ipv6_ready());
  link.state = WIFI_LINKSTATE_STA_DISCONNECTED;
  out = snapshot();
  assert(!out.connected && !out.count && !h2_bk_wifi_ipv6_ready());
  /* A radio-down snapshot also latches the loss before a delayed event. */
  observe_no_ready = 1;
  tick();
  observe_no_ready = 0;
  assert(!h2_bk_wifi_ipv6_ready());
  in_tcpip = 1;
  assert(h2_bk_wifi_ipv6_install(&station, &out, &status, &generation) ==
         ERR_OK);
  in_tcpip = 0;
  assert(!station.up && !station.link && !netif_default);
  for (unsigned i = 0; i < 3; i++)
    assert(station.state[i] == IP6_ADDR_INVALID);
  link.state = WIFI_LINKSTATE_STA_GOT_IP;
  station.up = 1;
  station.state[1] = IP6_ADDR_PREFERRED;
  memcpy(station.ip6[1].addr, ula, 16);
  connected_callback(NULL, EVENT_MOD_WIFI, EVENT_WIFI_STA_CONNECTED, NULL);
  out = snapshot();
  assert(h2_bk_wifi_ipv6_ready());
  link_error = BK_FAIL;
  out = (h2_bk_wifi_ipv6_snapshot_t){.size = sizeof(out)};
  assert(h2_bk_wifi_ipv6_snapshot(&out) == BK_FAIL && !h2_bk_wifi_ipv6_ready());
  link_error = 0;
  tcpip_error = 0;
  link_error = BK_FAIL;
  out = (h2_bk_wifi_ipv6_snapshot_t){.size = sizeof(out)};
  assert(h2_bk_wifi_ipv6_snapshot(&out) == BK_FAIL && !h2_bk_wifi_ipv6_ready());
  disconnected_callback(NULL, EVENT_MOD_WIFI, EVENT_WIFI_STA_DISCONNECTED,
                        NULL);
  assert(!h2_bk_wifi_ipv6_ready());
  assert(api_calls == 0);
  puts("BK IPv6 production nonblocking cache/snapshot/sync lifecycle PASS");
  return 0;
}
