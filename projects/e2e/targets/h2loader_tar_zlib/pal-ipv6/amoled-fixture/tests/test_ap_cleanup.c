#include "ipv6_ap.h"
#include "ap_test_sdk.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static esp_netif_t ap;
static struct netif iface;
static struct raw_pcb pcb;
static struct pbuf buffer;
static unsigned failure, ap_live, pcbs, sends, stops, starts;
static int fail_tcpip_exec;
static void (*queued)(void *), (*timer)(void *);
static void *queued_user, *timer_user;
static int ap_start(void *user, const h2_pal_wifi_ap_config_t *config,
                    uint32_t timeout) {
  (void)user; (void)timeout;
  assert(config && !ap_live);
  ++starts;
  if (failure == 1u)
    return H2_PAL_ERR_IO;
  ap_live = 1u;
  return H2_PAL_OK;
}
static int ap_stop(void *user, uint32_t timeout) {
  (void)user; (void)timeout;
  assert(ap_live && ((!pcbs && !timer) || fail_tcpip_exec));
  ++stops;
  ap_live = 0u;
  return H2_PAL_OK;
}
static int disconnect(void *user) { (void)user; return H2_PAL_OK; }
static int sleep_ms(void *user, uint32_t timeout) {
  (void)user; (void)timeout; return H2_PAL_OK;
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
  assert(strcmp(key, "WIFI_AP_DEF") == 0);
  return failure == 2u ? NULL : &ap;
}
esp_err_t esp_netif_create_ip6_linklocal(esp_netif_t *netif) {
  assert(netif == &ap);
  return failure == 3u ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_netif_add_ip6_address(esp_netif_t *netif,
                                   esp_ip6_addr_t address, bool preferred) {
  (void)address;
  assert(netif == &ap && preferred);
  return failure == 4u ? ESP_FAIL : ESP_OK;
}
struct netif *esp_netif_get_netif_impl(esp_netif_t *netif) {
  assert(netif == &ap);
  return failure == 5u ? NULL : &iface;
}
esp_err_t esp_netif_get_ip6_linklocal(esp_netif_t *netif, esp_ip6_addr_t *out) {
  assert(netif == &ap && out);
  memset(out, 0, sizeof(*out));
  return failure == 6u ? ESP_FAIL : ESP_OK;
}
int tcpip_callback(void (*callback)(void *), void *user) {
  if (failure == 7u)
    return ESP_FAIL;
  if (failure == 9u) {
    queued = callback;
    queued_user = user;
  } else {
    callback(user);
  }
  return ERR_OK;
}
esp_err_t esp_netif_tcpip_exec(esp_err_t (*callback)(void *), void *user) {
  if (fail_tcpip_exec)
    return ESP_FAIL;
  if (queued) {
    void (*pending)(void *) = queued;
    queued = NULL;
    pending(queued_user);
  }
  return callback(user);
}
int ip6addr_aton(const char *text, ip6_addr_t *out) {
  assert(text && out);
  memset(out, 0, sizeof(*out));
  return 1;
}
void ip6_addr_assign_zone(ip6_addr_t *ip, int type, struct netif *netif) {
  assert(ip && type == IP6_MULTICAST && netif == &iface);
}
struct pbuf *pbuf_alloc(int layer, size_t size, int type) {
  assert(layer == PBUF_IP && size == 56u && type == PBUF_RAM);
  return &buffer;
}
void pbuf_take(struct pbuf *value, const void *data, size_t size) {
  assert(value == &buffer && data && size == 56u);
}
void pbuf_free(struct pbuf *value) { assert(value == &buffer); }
struct raw_pcb *raw_new_ip_type(int type, int protocol) {
  assert(type == IPADDR_TYPE_V6 && protocol == IP6_NEXTH_ICMP6);
  if (failure == 8u)
    return NULL;
  ++pcbs;
  return &pcb;
}
void raw_bind_netif(struct raw_pcb *value, struct netif *netif) {
  assert(value == &pcb && netif == &iface);
}
void raw_set_multicast_ttl(struct raw_pcb *value, unsigned ttl) {
  assert(value == &pcb && ttl == 255u);
}
int raw_sendto_if_src(struct raw_pcb *value, struct pbuf *data,
    const ip_addr_t *destination, struct netif *netif, const ip_addr_t *source) {
  assert(value == &pcb && data == &buffer && destination && source && netif == &iface);
  ++sends;
  return ERR_OK;
}
void raw_remove(struct raw_pcb *value) { assert(value == &pcb && pcbs == 1u); --pcbs; }
void sys_timeout(uint32_t timeout, void (*callback)(void *), void *user) {
  assert(timeout == 6000u && !timer);
  timer = callback;
  timer_user = user;
}
void sys_untimeout(void (*callback)(void *), void *user) {
  assert(!timer || (timer == callback && timer_user == user));
  timer = NULL;
}
int main(void) {
  const h2_pal_wifi_ap_vtable_t ap_vtable = {.start = ap_start, .stop = ap_stop};
  const h2_pal_wifi_ap_api_t ap_api = {.vtable = &ap_vtable};
  const h2_pal_wifi_sta_vtable_t sta_vtable = {.disconnect = disconnect};
  const h2_pal_wifi_sta_api_t sta_api = {.vtable = &sta_vtable};
  const h2_pal_time_vtable_t time_vtable = {.sleep_ms = sleep_ms};
  const h2_pal_time_api_t time_api = {.vtable = &time_vtable};
  h2_runtime_t runtime = {.wifi_ap = &ap_api, .wifi_sta = &sta_api, .time = &time_api};
  for (failure = 1u; failure <= 9u; ++failure) {
    unsigned previous_stops = stops;
    assert(h2_ipv6_ap_start(&runtime) < 0);
    assert(!ap_live && !pcbs && !timer && !queued);
    assert(stops == previous_stops + (failure == 1u ? 0u : 1u));
  }
  failure = 0u;
  assert(h2_ipv6_ap_start(&runtime) == H2_PAL_OK);
  assert(ap_live && pcbs == 1u && timer && sends == 1u);
  assert(h2_ipv6_ap_start(&runtime) == H2_PAL_ERR_INVALID_STATE);
  void (*late)(void *) = timer;
  void *late_user = timer_user;
  assert(h2_ipv6_ap_stop(&runtime) == H2_PAL_OK);
  assert(!ap_live && !pcbs && !timer);
  late(late_user);
  assert(sends == 1u && !timer && !pcbs);
  assert(h2_ipv6_ap_stop(&runtime) == H2_PAL_OK);
  assert(h2_ipv6_ap_start(&runtime) == H2_PAL_OK);
  assert(h2_ipv6_ap_stop(&runtime) == H2_PAL_OK);
  assert(starts == 11u);
  assert(h2_ipv6_ap_start(&runtime) == H2_PAL_OK);
  unsigned previous_stops = stops, previous_sends = sends;
  late = timer;
  late_user = timer_user;
  fail_tcpip_exec = 1;
  assert(h2_ipv6_ap_stop(&runtime) == H2_PAL_ERR_IO);
  assert(ap_live == 0u && stops == previous_stops + 1u);
  assert(pcbs == 1u && timer != NULL);
  late(late_user);
  assert(sends == previous_sends);
  assert(h2_ipv6_ap_start(&runtime) == H2_PAL_ERR_INVALID_STATE);
  fail_tcpip_exec = 0;
  assert(h2_ipv6_ap_stop(&runtime) == H2_PAL_OK);
  assert(!pcbs && !timer && !ap_live && stops == previous_stops + 1u);
  assert(h2_ipv6_ap_start(&runtime) == H2_PAL_OK);
  assert(h2_ipv6_ap_stop(&runtime) == H2_PAL_OK);
  puts("AP startup cleanup: 9 failure paths, late callbacks and restart passed");
  return 0;
}
