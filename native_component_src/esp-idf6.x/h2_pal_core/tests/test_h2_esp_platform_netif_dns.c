#include "esp_netif.h"
#include "freertos/semphr.h"
#include "h2_esp_platform_core.h"
#include "lwip/dns.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static esp_netif_t s_wifi = {.key = "WIFI_STA_DEF", .index = 1};
static esp_netif_t *s_default;
static ip_addr_t s_servers[DNS_MAX_SERVERS];
static unsigned s_cache_clears;
static unsigned s_server_writes;
static int s_tcpip;
static unsigned s_tcpip_failures;
static unsigned s_pending_queries;
static unsigned s_terminated_queries;
static unsigned s_clear_logs;
static unsigned s_route_events;

esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_netif_tcpip_exec(esp_err_t (*callback)(void *), void *context) {
  assert(!s_tcpip);
  if (s_tcpip_failures != 0u) {
    --s_tcpip_failures;
    return ESP_FAIL;
  }
  s_tcpip = 1;
  esp_err_t result = callback(context);
  s_tcpip = 0;
  return result;
}
esp_netif_t *esp_netif_get_default_netif(void) {
  assert(s_tcpip);
  return s_default;
}
esp_netif_t *esp_netif_next_unsafe(esp_netif_t *netif) {
  return netif == NULL ? &s_wifi : NULL;
}
const char *esp_netif_get_ifkey(esp_netif_t *netif) { return netif->key; }
int esp_netif_get_netif_impl_index(esp_netif_t *netif) { return netif->index; }
bool esp_netif_is_netif_up(esp_netif_t *netif) {
  (void)netif;
  return true;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip) {
  (void)netif;
  memset(ip, 0, sizeof(*ip));
  return ESP_OK;
}
#if LWIP_IPV6
int esp_netif_get_all_ip6(esp_netif_t *netif, esp_ip6_addr_t *addresses) {
  assert(s_tcpip);
  assert(netif->ipv6_count >= 0 &&
         netif->ipv6_count <= CONFIG_LWIP_IPV6_NUM_ADDRESSES);
  memcpy(addresses, netif->ipv6,
         (size_t)netif->ipv6_count * sizeof(*addresses));
  return netif->ipv6_count;
}
#endif
esp_err_t esp_netif_get_mac(esp_netif_t *netif, uint8_t *mac) {
  (void)netif;
  memset(mac, 0, 6u);
  return ESP_OK;
}
esp_err_t esp_netif_get_mtu(esp_netif_t *netif, uint16_t *mtu) {
  (void)netif;
  *mtu = 1500u;
  return ESP_OK;
}
esp_err_t esp_netif_get_dns_info(esp_netif_t *netif, esp_netif_dns_type_t type,
                                 esp_netif_dns_info_t *dns) {
  assert(s_tcpip);
  *dns = netif->dns[type];
  return netif->dns_result[type];
}
esp_err_t esp_netif_set_default_netif(esp_netif_t *netif) {
  s_default = netif;
  return ESP_OK;
}
size_t test_netif_strlcpy(char *dst, const char *src, size_t size) {
  size_t length = strlen(src);
  if (size != 0u) {
    size_t copied = length < size ? length : size - 1u;
    memcpy(dst, src, copied);
    dst[copied] = '\0';
  }
  return length;
}
char *ipaddr_ntoa_r(const ip_addr_t *ip, char *buffer, int length) {
  (void)ip;
  assert(length > 0);
  buffer[0] = '\0';
  return buffer;
}
void test_netif_log(const char *tag, const char *format, ...) {
  (void)tag;
  if (strstr(format, "cache=cleared") != NULL)
    ++s_clear_logs;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
  return storage;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
  (void)timeout;
  assert(semaphore != NULL);
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  assert(semaphore != NULL);
  return pdTRUE;
}
const ip_addr_t *dns_getserver(u8_t index) {
  assert(s_tcpip && index < DNS_MAX_SERVERS);
  return &s_servers[index];
}
void dns_setserver(u8_t index, const ip_addr_t *server) {
  assert(s_tcpip && index < DNS_MAX_SERVERS);
  s_servers[index] = *server;
  ++s_server_writes;
}
void dns_clear_cache(void) {
  assert(s_tcpip);
  ++s_cache_clears;
  s_terminated_queries += s_pending_queries;
  s_pending_queries = 0u;
}
static h2_pal_result_t post_event(void *user,
                                  const h2_pal_system_event_t *event,
                                  uint32_t timeout_ms) {
  (void)user;
  assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED);
  ++s_route_events;
  (void)timeout_ms;
  return H2_PAL_OK;
}
const h2_pal_system_event_api_t *h2_esp_platform_system_event_api(void) {
  static const h2_pal_system_event_vtable_t vtable = {.post = post_event};
  static const h2_pal_system_event_api_t api = {.vtable = &vtable};
  return &api;
}
static void sdk_set_dns(esp_netif_t *netif, u8_t index, uint32_t address) {
  memset(&netif->dns[index], 0, sizeof(netif->dns[index]));
  netif->dns[index].ip.u_addr.ip4.addr = address;
  memset(&s_servers[index], 0, sizeof(s_servers[index]));
#if LWIP_IPV4 && LWIP_IPV6
  s_servers[index].u_addr.ip4.addr = address;
#else
  s_servers[index].addr = address;
#endif
}

static void begin_monitor(void) {
  h2_esp_platform_netif_monitor_deinit();
  memset(s_wifi.dns, 0, sizeof(s_wifi.dns));
  memset(s_wifi.dns_result, 0, sizeof(s_wifi.dns_result));
  memset(s_servers, 0, sizeof(s_servers));
#if LWIP_IPV6
  memset(s_wifi.ipv6, 0, sizeof(s_wifi.ipv6));
  s_wifi.ipv6_count = 0;
#endif
  s_default = &s_wifi;
  sdk_set_dns(&s_wifi, 0u, 0x01010101u);
  s_cache_clears = 0u;
  s_server_writes = 0u;
  s_clear_logs = 0u;
  s_route_events = 0u;
  s_pending_queries = 0u;
  s_terminated_queries = 0u;
  assert(h2_esp_platform_netif_monitor_init() == H2_PAL_OK);
}

static void expect_reconcile(unsigned clears, unsigned writes) {
  assert(h2_esp_platform_netif_reconcile_default() == H2_PAL_OK);
  assert(s_cache_clears == clears);
  assert(s_clear_logs == clears);
  assert(s_server_writes == writes);
}

static void test_sdk_updated_both_views(void) {
  begin_monitor();
  expect_reconcile(1u, 0u);
  /* Same default and both views already B: there is no corrective write. */
  sdk_set_dns(&s_wifi, 0u, 0x08080808u);
  sdk_set_dns(&s_wifi, 1u, 0x09090909u);
  s_pending_queries = 2u;
  expect_reconcile(2u, 0u);
  assert(s_terminated_queries == 2u);
  s_pending_queries = 1u;
  expect_reconcile(2u, 0u);
  assert(s_pending_queries == 1u);
  assert(h2_esp_platform_netif_monitor_init() == H2_PAL_OK);
  expect_reconcile(2u, 0u);
}

static void test_default_switch_and_loss(void) {
  begin_monitor();
  expect_reconcile(1u, 0u);
  esp_netif_t ppp = s_wifi;
  ppp.index = 2;
  ppp.key = "PPP_DEF";
  s_default = &ppp;
  expect_reconcile(2u, 0u);
  expect_reconcile(2u, 0u);
  s_default = NULL;
  expect_reconcile(3u, 0u);
  expect_reconcile(3u, 0u);
  s_default = &s_wifi;
  expect_reconcile(4u, 0u);
  h2_esp_platform_netif_monitor_deinit();
}

static void test_no_default_first_sync(void) {
  begin_monitor();
  s_default = NULL;
  expect_reconcile(0u, 0u);
  expect_reconcile(0u, 0u);
  s_default = &s_wifi;
  expect_reconcile(1u, 0u);
}

static void test_lifecycle_reset(void) {
  begin_monitor();
  expect_reconcile(1u, 0u);
  h2_esp_platform_netif_monitor_deinit();
  /* The new monitor sees the same pointer and servers, but old answers may
   * have been cached while the monitor was stopped. Flush once on recovery. */
  s_tcpip_failures = 1u;
  assert(h2_esp_platform_netif_monitor_init() == H2_PAL_ERR_IO);
  assert(s_cache_clears == 1u);
  assert(h2_esp_platform_netif_monitor_init() == H2_PAL_OK);
  expect_reconcile(2u, 0u);
  expect_reconcile(2u, 0u);
  s_tcpip_failures = 1u;
  h2_esp_platform_netif_monitor_deinit();
  assert(h2_esp_platform_netif_monitor_init() == H2_PAL_OK);
  expect_reconcile(3u, 0u);
}

static void test_default_query_failure(void) {
  begin_monitor();
  expect_reconcile(1u, 0u);
  sdk_set_dns(&s_wifi, 0u, 0x08080808u);
  s_tcpip_failures = 1u;
  assert(h2_esp_platform_netif_reconcile_default() == H2_PAL_ERR_IO);
  assert(s_cache_clears == 1u);
  int index = s_wifi.index;
  const char *key = s_wifi.key;
  s_wifi.index = 0;
  s_wifi.key = "";
  assert(h2_esp_platform_netif_reconcile_default() == H2_PAL_ERR_NOT_FOUND);
  assert(s_cache_clears == 1u);
  s_wifi.index = index;
  s_wifi.key = key;
  expect_reconcile(2u, 0u);
}

static void test_fallback_preserved(void) {
  begin_monitor();
  sdk_set_dns(&s_wifi, DNS_FALLBACK_SERVER_INDEX, 0x09090909u);
  memset(&s_wifi.dns[DNS_FALLBACK_SERVER_INDEX], 0,
         sizeof(s_wifi.dns[DNS_FALLBACK_SERVER_INDEX]));
  ip_addr_t fallback = s_servers[DNS_FALLBACK_SERVER_INDEX];
  expect_reconcile(1u, 0u);
  assert(ip_addr_cmp(&s_servers[DNS_FALLBACK_SERVER_INDEX], &fallback));
  expect_reconcile(1u, 0u);
  /* A changed configured global fallback is part of the effective set even
   * though the interface still has no fallback. */
  sdk_set_dns(&s_wifi, DNS_FALLBACK_SERVER_INDEX, 0x04040404u);
  memset(&s_wifi.dns[DNS_FALLBACK_SERVER_INDEX], 0,
         sizeof(s_wifi.dns[DNS_FALLBACK_SERVER_INDEX]));
  expect_reconcile(2u, 0u);
#if CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
  s_wifi.dns[DNS_FALLBACK_SERVER_INDEX].ip.u_addr.ip4.addr = 0x08080808u;
  expect_reconcile(3u, 1u);
  expect_reconcile(3u, 1u);
#endif
}

#if CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
static void test_resolver_correction(void) {
  begin_monitor();
  expect_reconcile(1u, 0u);
  memset(&s_servers[0], 0, sizeof(s_servers[0]));
  /* Repairing a DHCP-cleared global must still invalidate cached answers
   * even though the effective set equals the previous snapshot. */
  expect_reconcile(2u, 1u);
  expect_reconcile(2u, 1u);
  memset(&s_wifi.dns[0], 0, sizeof(s_wifi.dns[0]));
  expect_reconcile(3u, 2u);
  assert(ip_addr_isany(&s_servers[0]));
}

static void test_incomplete_and_invalid_reads(void) {
  begin_monitor();
  s_wifi.dns_result[1] = ESP_FAIL;
  expect_reconcile(0u, 0u);
  s_wifi.dns_result[1] = ESP_OK;
  expect_reconcile(1u, 0u);
  /* Slot zero differs, but a later slot fails. Neither partial corrective
   * writes nor partial baseline commits are permitted. */
  s_wifi.dns[0].ip.u_addr.ip4.addr = 0x08080808u;
  s_wifi.dns_result[1] = ESP_FAIL;
  s_pending_queries = 1u;
  expect_reconcile(1u, 0u);
  expect_reconcile(1u, 0u);
  assert(s_pending_queries == 1u);
  s_wifi.dns_result[1] = ESP_OK;
  expect_reconcile(2u, 1u);
  assert(s_terminated_queries == 1u);
  sdk_set_dns(&s_wifi, 0u, 0x04040404u);
  s_wifi.dns[1].ip.type = 99u;
  expect_reconcile(2u, 1u);
#if !LWIP_IPV6
  s_wifi.dns[1].ip.type = ESP_IPADDR_TYPE_V6;
  expect_reconcile(2u, 1u);
#endif
  s_wifi.dns[1].ip.type = ESP_IPADDR_TYPE_V4;
  expect_reconcile(3u, 1u);
  /* An incomplete read during an interface switch does not consume the
   * previous complete DNS baseline. The subsequent valid snapshot flushes. */
  esp_netif_t ppp = s_wifi;
  ppp.index = 2;
  ppp.key = "PPP_DEF";
  ppp.dns_result[1] = ESP_FAIL;
  s_default = &ppp;
  expect_reconcile(3u, 1u);
  assert(s_route_events == 1u);
  ppp.dns_result[1] = ESP_OK;
  expect_reconcile(4u, 1u);
  assert(s_route_events == 1u);
  s_default = &s_wifi;
}
#endif

#if LWIP_IPV4 && LWIP_IPV6
static void test_ipv6_netif_and_dns_scope(void) {
  begin_monitor();
  const uint8_t link_local[16] = {0xfeu, 0x80u, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 1u};
  const uint8_t global[16] = {0xfdu, 0x53u, 0, 0, 0, 0, 0, 0,
                            0, 0, 0, 0, 0, 0, 0, 2u};
  memcpy(s_wifi.ipv6[0].addr, link_local, sizeof(link_local));
  s_wifi.ipv6_count = 1;
  s_wifi.dns[1].ip.type = ESP_IPADDR_TYPE_V6;
  memcpy(s_wifi.dns[1].ip.u_addr.ip6.addr, link_local, sizeof(link_local));
  h2_pal_netif_status_t status;
  const h2_pal_netif_api_t *api = h2_esp_platform_netif_api();
  assert(h2_pal_netif_get_status(api, NULL, &status) == H2_PAL_OK);
  assert((status.flags & H2_PAL_NETIF_FLAG_HAS_IPV6) != 0u);
  assert(status.ipv6.family == H2_PAL_NET_FAMILY_IPV6);
  assert(status.ipv6.scope_id == (uint32_t)s_wifi.index);
  assert(memcmp(status.ipv6.ip, link_local, sizeof(link_local)) == 0);
  assert(status.dns_count == 2u);
  assert(status.dns[1].addr.family == H2_PAL_NET_FAMILY_IPV6);
  assert(status.dns[1].addr.scope_id == (uint32_t)s_wifi.index);
  assert(memcmp(status.dns[1].addr.ip, link_local, sizeof(link_local)) == 0);
  memcpy(s_wifi.ipv6[1].addr, global, sizeof(global));
  s_wifi.ipv6_count = 2;
  memcpy(s_wifi.dns[1].ip.u_addr.ip6.addr, global, sizeof(global));
  assert(h2_pal_netif_get_status(api, NULL, &status) == H2_PAL_OK);
  assert(status.ipv6.scope_id == 0u);
  assert(memcmp(status.ipv6.ip, global, sizeof(global)) == 0);
  assert(status.dns[1].addr.scope_id == 0u);
  s_wifi.dns_result[1] = ESP_FAIL;
  assert(h2_pal_netif_get_status(api, NULL, &status) == H2_PAL_OK);
  assert(status.dns_count == 1u);
}

static void test_address_family_and_ipv6(void) {
  begin_monitor();
  expect_reconcile(1u, 0u);
  /* IPv4's unused union words are not part of its address. */
  s_wifi.dns[0].ip.u_addr.ip6.addr[3] = 17u;
  expect_reconcile(1u, 0u);
  s_wifi.dns[0].ip.u_addr.ip6.addr[3] = 0u;
  /* Identical first address word, different family; compare typed addresses. */
  s_wifi.dns[0].ip.type = ESP_IPADDR_TYPE_V6;
  s_servers[0].type = ESP_IPADDR_TYPE_V6;
  expect_reconcile(2u, 0u);
  s_wifi.dns[0].ip.u_addr.ip6.addr[3] = 8u;
  s_servers[0].u_addr.ip6.addr[3] = 8u;
  expect_reconcile(3u, 0u);
  expect_reconcile(3u, 0u);
}
#endif

int main(void) {
  test_sdk_updated_both_views();
  test_default_switch_and_loss();
  test_no_default_first_sync();
  test_lifecycle_reset();
  test_default_query_failure();
  test_fallback_preserved();
#if CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
  test_resolver_correction();
  test_incomplete_and_invalid_reads();
#endif
#if LWIP_IPV4 && LWIP_IPV6
  test_ipv6_netif_and_dns_scope();
  test_address_family_and_ipv6();
#endif
  h2_esp_platform_netif_monitor_deinit();
  puts("netif DNS tests passed");
  return 0;
}
