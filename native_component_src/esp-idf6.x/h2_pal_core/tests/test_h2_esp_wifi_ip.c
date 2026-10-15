#include "h2_esp_wifi_ip.h"
#include "lwip/netif.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

const ip6_addr_t test_ip6_zero;
static unsigned initialized, cleared;
static int in_tcpip, callback_error;
void *esp_netif_get_netif_impl(esp_netif_t *n) { return &n->n; }
int esp_netif_is_netif_up(esp_netif_t *n) { return n->n.up; }
int esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *out) {
  *out = n->info;
  return 0;
}
int esp_netif_get_all_preferred_ip6(esp_netif_t *n, esp_ip6_addr_t *out) {
  int count = 0;
  for (unsigned i = 0; i < 3; i++)
    if (n->n.state[i] == IP6_ADDR_PREFERRED)
      memcpy(out[count++].addr, n->n.ip6[i].addr, 16);
  return count;
}
err_t tcpip_callback_wait(void (*fn)(void *), void *arg) {
  if (callback_error)
    return ERR_IF;
  in_tcpip = 1;
  fn(arg);
  in_tcpip = 0;
  return ERR_OK;
}
void netif_ip6_addr_set_state(struct netif *n, int i, unsigned s) {
  assert(in_tcpip);
  n->state[i] = s;
}
void netif_ip6_addr_set(struct netif *n, int i, const ip6_addr_t *a) {
  assert(in_tcpip);
  n->ip6[i] = *a;
}
void nd6_cleanup_netif(struct netif *n) {
  (void)n;
  assert(in_tcpip);
  ++cleared;
}
void netif_create_ip6_linklocal_address(struct netif *n, int from_mac) {
  assert(in_tcpip && from_mac);
  const uint8_t ll[] = {0xfe, 0x80};
  memcpy(n->ip6[0].addr, ll, sizeof(ll));
  n->state[0] = 1;
  ++initialized;
}
int main(void) {
  esp_netif_t n = {.n = {.up = 1}};
  assert(h2_esp_wifi_ip_start(&n) == H2_PAL_OK);
  assert(initialized == 1 && cleared == 1 && n.n.autoconfig == 1);
  h2_pal_wifi_sta_status_t status = {.state = H2_PAL_WIFI_STA_STATE_GOT_IP};
  n.n.state[0] = IP6_ADDR_PREFERRED;
  h2_esp_wifi_ip_snapshot(&n, &status);
  assert(!status.ip.ip6_valid && !status.ip_valid &&
         !h2_pal_wifi_sta_status_has_ip(&status));
  const uint8_t ula[16] = {0xfd, 0x53, 0, 0, 0, 0, 0, 0,
                           0,    0,    0, 0, 0, 0, 0, 1};
  memcpy(n.n.ip6[1].addr, ula, 16);
  n.n.state[1] = IP6_ADDR_PREFERRED;
  h2_esp_wifi_ip_snapshot(&n, &status);
  assert(status.ip.ip6_valid && !status.ip_valid &&
         h2_pal_wifi_sta_status_has_ip(&status));
  n.info.ip.addr = htonl(0xc0a80402);
  h2_esp_wifi_ip_snapshot(&n, &status);
  assert(status.ip_valid && status.ip.ip4 == 0xc0a80402 && status.ip.ip6_valid);
  n.info.ip.addr = 0;
  h2_esp_wifi_ip_snapshot(&n, &status);
  assert(!status.ip_valid && status.ip.ip6_valid);
  n.n.state[1] = 0x10;
  h2_esp_wifi_ip_snapshot(&n, &status);
  assert(!status.ip.ip6_valid && !h2_pal_wifi_sta_status_has_ip(&status));
  n.n.state[1] = IP6_ADDR_PREFERRED;
  h2_esp_wifi_ip_stop(&n);
  assert(!n.n.autoconfig && n.n.state[0] == IP6_ADDR_INVALID &&
         n.n.state[1] == IP6_ADDR_INVALID);
  assert(h2_esp_wifi_ip_start(&n) == H2_PAL_OK && initialized == 2);
  n.n.up = 0;
  h2_esp_wifi_ip_snapshot(&n, &status);
  assert(!status.ip_valid && !status.ip.ip6_valid);
  assert(h2_esp_wifi_ip_start(&n) == H2_PAL_ERR_UNAVAILABLE);
  n.n.up = 1;
  callback_error = 1;
  assert(h2_esp_wifi_ip_start(&n) == H2_PAL_ERR_IO);
  assert(h2_esp_wifi_ip_start(NULL) == H2_PAL_ERR_INVALID_ARG);
  puts("ESP production Wi-Fi IPv6 lifecycle PASS");
  return 0;
}
