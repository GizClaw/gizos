#include "h2_esp_wifi_ip.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
const ip6_addr_t test_ip6_zero;
void *esp_netif_get_netif_impl(esp_netif_t *n) { return &n->n; }
int esp_netif_is_netif_up(esp_netif_t *n) { return n->n.up; }
int esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *out) {
  *out = n->info;
  return 0;
}
int esp_netif_get_all_preferred_ip6(esp_netif_t *n, esp_ip6_addr_t *out) {
  (void)n;
  (void)out;
  return 0;
}
err_t tcpip_callback_wait(void (*fn)(void *), void *arg) {
  fn(arg);
  return ERR_OK;
}
void netif_ip6_addr_set_state(struct netif *n, int i, unsigned s) {
  n->state[i] = s;
}
void netif_ip6_addr_set(struct netif *n, int i, const ip6_addr_t *a) {
  n->ip6[i] = *a;
}
void nd6_cleanup_netif(struct netif *n) { (void)n; }
int main(void) {
  esp_netif_t n = {.n = {.up = 1}, .info = {.ip = {.addr = htonl(0xc0000201)}}};
  assert(h2_esp_wifi_ip_start(&n) == H2_PAL_ERR_UNSUPPORTED);
  h2_pal_wifi_sta_status_t status = {.state = H2_PAL_WIFI_STA_STATE_GOT_IP};
  h2_esp_wifi_ip_snapshot(&n, &status);
  assert(status.ip_valid && status.ip.ip4 == 0xc0000201 &&
         !status.ip.ip6_valid);
  h2_esp_wifi_ip_stop(&n);
  puts("ESP production IPv6/SLAAC-disabled compilation/IPv4 PASS");
  return 0;
}
