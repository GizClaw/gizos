#include "ap_test_sdk.h"
#include "iperf_ap.h"
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
static unsigned starts, stops, sleeps, pcbs, radio_live;
static int start_error, stop_error, missing_netif;
static void (*timer)(void *);
static void *timer_user;
static int ap_start(void *user, const h2_pal_wifi_ap_config_t *config,
                    uint32_t timeout) {
  (void)user;
  assert(config && timeout == 5000u && !radio_live);
  ++starts;
  if (!start_error)
    radio_live = 1u;
  return start_error;
}
static int ap_stop(void *user, uint32_t timeout) {
  (void)user;
  assert(timeout == 5000u && radio_live && !pcbs && !timer);
  ++stops;
  if (!stop_error)
    radio_live = 0u;
  return stop_error;
}
static int disconnect(void *user) {
  (void)user;
  return H2_PAL_OK;
}
static int sleep_ms(void *user, uint32_t timeout) {
  (void)user;
  (void)timeout;
  ++sleeps;
  /* A persistent stop fault must return instead of polling forever. */
  assert(stop_error == H2_PAL_OK);
  return H2_PAL_OK;
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
  assert(strcmp(key, "WIFI_AP_DEF") == 0);
  return missing_netif ? NULL : &ap;
}
esp_err_t esp_netif_dhcps_stop(esp_netif_t *value) {
  assert(value == &ap);
  return ESP_OK;
}
esp_err_t esp_netif_set_ip_info(esp_netif_t *value,
                                const esp_netif_ip_info_t *info) {
  assert(value == &ap && info);
  return ESP_OK;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *value, esp_netif_ip_info_t *out) {
  assert(value == &ap && out);
  out->ip.addr = 0xc0a80401u;
  return ESP_OK;
}
esp_err_t esp_netif_create_ip6_linklocal(esp_netif_t *value) {
  assert(value == &ap);
  return ESP_OK;
}
esp_err_t esp_netif_add_ip6_address(esp_netif_t *value, esp_ip6_addr_t address,
                                    bool preferred) {
  (void)address;
  assert(value == &ap && preferred);
  return ESP_OK;
}
esp_err_t esp_netif_get_ip6_linklocal(esp_netif_t *value, esp_ip6_addr_t *out) {
  assert(value == &ap && out);
  memset(out, 0, sizeof(*out));
  return ESP_OK;
}
struct netif *esp_netif_get_netif_impl(esp_netif_t *value) {
  assert(value == &ap);
  return &iface;
}
int ip6addr_aton(const char *text, ip6_addr_t *out) {
  assert(text && out);
  memset(out, 0, sizeof(*out));
  return 1;
}
void ip6_addr_assign_zone(ip6_addr_t *ip, int type, struct netif *value) {
  assert(ip && type == IP6_MULTICAST && value == &iface);
}
struct pbuf *pbuf_alloc(int layer, size_t size, int type) {
  assert(layer == PBUF_IP && size == 56u && type == PBUF_RAM);
  return &buffer;
}
int pbuf_take(struct pbuf *value, const void *data, size_t size) {
  assert(value == &buffer && data && size == 56u);
  return ERR_OK;
}
void pbuf_free(struct pbuf *value) { assert(value == &buffer); }
struct raw_pcb *raw_new_ip_type(int type, int protocol) {
  assert(type == IPADDR_TYPE_V6 && protocol == IP6_NEXTH_ICMP6 && !pcbs);
  pcbs = 1u;
  return &pcb;
}
void raw_bind_netif(struct raw_pcb *value, struct netif *netif) {
  assert(value == &pcb && netif == &iface);
}
void raw_set_multicast_ttl(struct raw_pcb *value, unsigned ttl) {
  assert(value == &pcb && ttl == 255u);
}
int raw_sendto_if_src(struct raw_pcb *value, struct pbuf *data,
                      const ip_addr_t *dest, struct netif *netif,
                      const ip_addr_t *source) {
  assert(value == &pcb && data == &buffer && dest && source && netif == &iface);
  return ERR_OK;
}
void raw_remove(struct raw_pcb *value) {
  assert(value == &pcb && pcbs);
  pcbs = 0u;
}
void sys_timeout(uint32_t timeout, void (*callback)(void *), void *user) {
  assert(timeout == 3000u);
  timer = callback;
  timer_user = user;
}
void sys_untimeout(void (*callback)(void *), void *user) {
  assert(!timer || (timer == callback && timer_user == user));
  timer = NULL;
  timer_user = NULL;
}
int tcpip_callback_wait(void (*callback)(void *), void *user) {
  callback(user);
  return ERR_OK;
}

int main(void) {
  const h2_pal_wifi_sta_vtable_t sta_vtable = {.disconnect = disconnect};
  const h2_pal_wifi_sta_api_t sta = {NULL, &sta_vtable};
  const h2_pal_wifi_ap_vtable_t ap_vtable = {.start = ap_start,
                                             .stop = ap_stop};
  const h2_pal_wifi_ap_api_t api = {NULL, &ap_vtable};
  const h2_pal_time_vtable_t time_vtable = {.sleep_ms = sleep_ms};
  const h2_pal_time_api_t time = {NULL, &time_vtable};
  h2_runtime_t runtime = {.wifi_sta = &sta, .wifi_ap = &api, .time = &time};
  h2_iperf_amoled_ap_t state = {.runtime = &runtime};
  h2_iperf_server_app_network_t network;
  assert(h2_iperf_amoled_ap_stop(&state) == H2_PAL_OK && stops == 0u);
  start_error = H2_PAL_ERR_IO;
  assert(h2_iperf_amoled_ap_start(&state, H2_IPERF_SERVER_APP_MODE_DUAL,
                                  &network) == H2_PAL_ERR_IO);
  assert(!state.active && !radio_live && stops == 0u);
  start_error = H2_PAL_OK;
  missing_netif = 1;
  stop_error = H2_PAL_ERR_TIMEOUT;
  memset(&network, 0xa5, sizeof(network));
  assert(h2_iperf_amoled_ap_start(&state, H2_IPERF_SERVER_APP_MODE_DUAL,
                                  &network) == H2_PAL_ERR_TIMEOUT);
  const h2_iperf_server_app_network_t empty = {0};
  assert(!memcmp(&network, &empty, sizeof(network)) && state.active &&
         radio_live);
  assert(stops == 1u && sleeps == 0u);
  assert(h2_iperf_amoled_ap_start(&state, H2_IPERF_SERVER_APP_MODE_IPV4,
                                  &network) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_iperf_amoled_ap_stop(&state) == H2_PAL_ERR_TIMEOUT &&
         state.active && stops == 2u);
  stop_error = H2_PAL_OK;
  assert(h2_iperf_amoled_ap_stop(&state) == H2_PAL_OK && !state.active &&
         !radio_live && stops == 3u);
  assert(h2_iperf_amoled_ap_stop(&state) == H2_PAL_OK && stops == 3u);
  missing_netif = 0;
  for (unsigned i = 0; i < 3u; ++i) {
    const h2_iperf_server_app_mode_t modes[] = {H2_IPERF_SERVER_APP_MODE_IPV4,
                                                H2_IPERF_SERVER_APP_MODE_IPV6,
                                                H2_IPERF_SERVER_APP_MODE_DUAL};
    assert(h2_iperf_amoled_ap_start(&state, modes[i], &network) == H2_PAL_OK);
    assert(state.active && radio_live && pcbs == (i != 0u));
    assert(h2_iperf_amoled_ap_stop(&state) == H2_PAL_OK && !state.active &&
           !radio_live && !pcbs && !timer);
  }
  assert(starts == 5u && stops == 6u);
  puts("iperf AP PASS: bounded partial-start failure, retained ownership, stop "
       "retry and three modes");
  return 0;
}
