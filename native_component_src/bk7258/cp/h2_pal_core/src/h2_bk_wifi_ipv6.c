#include "h2_bk_wifi_ipv6.h"
#include "h2/pal/net/h2_pal_net.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/timeouts.h"
#include "net.h"
#include <components/event.h>
#include <modules/wifi.h>
#include <os/os.h>
#include <string.h>

static uint32_t generation = 1u, associated_epoch, association_observed;
static uint32_t ready_epoch, cache_guard, cache_valid;
static uint32_t
    cache_words[sizeof(h2_bk_wifi_ipv6_snapshot_t) / sizeof(uint32_t)];
static int registered;
_Static_assert(sizeof(h2_bk_wifi_ipv6_snapshot_t) % sizeof(uint32_t) == 0u,
               "fixed snapshot word layout");

static bk_err_t h2_bk_wifi_ipv6_association(void *arg, event_module_t module,
                                            int event, void *data) {
  (void)arg;
  (void)module;
  (void)data;
  __atomic_store_n(&association_observed, 1u, __ATOMIC_RELEASE);
  __atomic_store_n(&associated_epoch, 0u, __ATOMIC_RELEASE);
  uint32_t epoch = __atomic_add_fetch(&generation, 1u, __ATOMIC_ACQ_REL);
  __atomic_store_n(&ready_epoch, 0u, __ATOMIC_RELEASE);
  if (event == EVENT_WIFI_STA_CONNECTED)
    __atomic_store_n(&associated_epoch, epoch, __ATOMIC_RELEASE);
  return BK_OK;
}

/* TCP/IP is the sole address-cache writer. CIF RPC never waits on TCP/IP:
 * DHCP/ND callbacks can themselves send CIF events, so such a wait cycles. */
static void h2_bk_wifi_ipv6_refresh_cache(void *user) {
  (void)user;
  h2_bk_wifi_ipv6_snapshot_t snapshot = {0};
  snapshot.generation = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
  uint32_t associated =
      __atomic_load_n(&associated_epoch, __ATOMIC_ACQUIRE);
  uint32_t ready = 0u;
#if LWIP_IPV6
  struct netif *sta = (struct netif *)net_get_sta_handle();
  if (associated != 0u && associated == snapshot.generation &&
      sta != NULL && netif_is_up(sta)) {
    for (unsigned i = 0u; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
      if (!ip6_addr_ispreferred(netif_ip6_addr_state(sta, i)))
        continue;
      if (snapshot.count == H2_BK_WIFI_IPV6_MAX)
        break;
      h2_bk_wifi_ipv6_address_t *address =
          &snapshot.addresses[snapshot.count++];
      memcpy(address->address, netif_ip6_addr(sta, i)->addr, 16u);
      address->preferred = 1u;
      ready |= h2_pal_net_ipv6_is_non_link_local_unicast(address->address);
    }
  }
#endif
  /* A disconnect can leave lwIP up with the old preferred addresses. Neither
   * a later refresh nor a capture interrupted by an association event may
   * publish those addresses as belonging to the new radio association. */
  if (associated != snapshot.generation ||
      snapshot.generation != __atomic_load_n(&generation, __ATOMIC_ACQUIRE) ||
      snapshot.generation !=
          __atomic_load_n(&associated_epoch, __ATOMIC_ACQUIRE)) {
    snapshot.count = 0u;
    memset(snapshot.addresses, 0, sizeof(snapshot.addresses));
    ready = 0u;
  }
  uint32_t words[sizeof(cache_words) / sizeof(cache_words[0])];
  memcpy(words, &snapshot, sizeof(words));
  uint32_t before = __atomic_load_n(&cache_guard, __ATOMIC_SEQ_CST);
  __atomic_store_n(&cache_guard, before + 1u, __ATOMIC_SEQ_CST);
  for (unsigned i = 0u; i < sizeof(words) / sizeof(words[0]); ++i)
    __atomic_store_n(&cache_words[i], words[i], __ATOMIC_SEQ_CST);
  __atomic_store_n(&cache_guard, before + 2u, __ATOMIC_SEQ_CST);
  __atomic_store_n(&cache_valid, 1u, __ATOMIC_RELEASE);
  /* One word publishes validity and epoch together. Separate stores can pair
   * a new empty epoch with the previous writer's ready bit. Zero is not ready. */
  __atomic_store_n(&ready_epoch, ready ? snapshot.generation : 0u,
                   __ATOMIC_RELEASE);
  sys_timeout(250u, h2_bk_wifi_ipv6_refresh_cache, NULL);
}

static int h2_bk_wifi_ipv6_cache_read(h2_bk_wifi_ipv6_snapshot_t *out) {
  if (!__atomic_load_n(&cache_valid, __ATOMIC_ACQUIRE))
    return BK_ERR_BUSY;
  uint32_t words[sizeof(cache_words) / sizeof(cache_words[0])];
  for (unsigned attempt = 0u; attempt < 3u; ++attempt) {
    uint32_t before = __atomic_load_n(&cache_guard, __ATOMIC_SEQ_CST);
    if (before & 1u)
      continue;
    for (unsigned i = 0u; i < sizeof(words) / sizeof(words[0]); ++i)
      words[i] = __atomic_load_n(&cache_words[i], __ATOMIC_SEQ_CST);
    uint32_t after = __atomic_load_n(&cache_guard, __ATOMIC_SEQ_CST);
    if (before == after && !(after & 1u)) {
      memcpy(out, words, sizeof(*out));
      return BK_OK;
    }
  }
  return BK_ERR_BUSY;
}

int h2_bk_wifi_ipv6_snapshot(h2_bk_wifi_ipv6_snapshot_t *out) {
  if (out == NULL || out->size != sizeof(*out))
    return BK_ERR_PARAM;
  memset(out, 0, sizeof(*out));
  int start_cache = !registered;
  if (start_cache) {
    bk_err_t rc = bk_event_register_cb(EVENT_MOD_WIFI, EVENT_WIFI_STA_CONNECTED,
                                       h2_bk_wifi_ipv6_association, NULL);
    if (rc != BK_OK && rc != BK_ERR_EVENT_CB_EXIST)
      return rc;
    rc = bk_event_register_cb(EVENT_MOD_WIFI, EVENT_WIFI_STA_DISCONNECTED,
                              h2_bk_wifi_ipv6_association, NULL);
    if (rc != BK_OK && rc != BK_ERR_EVENT_CB_EXIST)
      return rc;
  }
  uint32_t before = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
  wifi_link_status_t link = {0};
  int rc = bk_wifi_sta_get_link_status(&link);
  if (rc != BK_OK)
    goto failure;
  if (start_cache) {
    /* The first RPC may arrive after the radio already connected. Seed only
     * before any observed event; a stale link-status reply must never undo a
     * disconnect. CAS preserves a newer association event racing this seed. */
    if ((link.state == WIFI_LINKSTATE_STA_CONNECTED ||
         link.state == WIFI_LINKSTATE_STA_GOT_IP) &&
        !__atomic_load_n(&association_observed, __ATOMIC_ACQUIRE) &&
        before == __atomic_load_n(&generation, __ATOMIC_ACQUIRE)) {
      uint32_t empty = 0u;
      (void)__atomic_compare_exchange_n(&associated_epoch, &empty, before, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    if (tcpip_callback(h2_bk_wifi_ipv6_refresh_cache, NULL) != ERR_OK) {
      rc = BK_ERR_BUSY;
      goto failure;
    }
    registered = 1;
  }
  out->generation = before;
  if ((link.state == WIFI_LINKSTATE_STA_CONNECTED ||
       link.state == WIFI_LINKSTATE_STA_GOT_IP) &&
      __atomic_load_n(&associated_epoch, __ATOMIC_ACQUIRE) == before) {
    h2_bk_wifi_ipv6_snapshot_t cached;
    rc = h2_bk_wifi_ipv6_cache_read(&cached);
    if (rc != BK_OK || cached.generation != before) {
      rc = BK_ERR_BUSY;
      goto failure;
    }
    out->count = cached.count;
    memcpy(out->addresses, cached.addresses, sizeof(out->addresses));
    size_t length = 0u;
    while (length < sizeof(out->ssid) && link.ssid[length] != '\0')
      ++length;
    memcpy(out->ssid, link.ssid, length);
    out->ssid_len = (uint32_t)length;
    memcpy(out->bssid, link.bssid, sizeof(out->bssid));
    out->connected = 1u;
    out->channel = link.channel;
    out->rssi = link.rssi;
  } else {
    /* This RPC's radio observation may precede the event callback. Do not
     * clear a newer association if the status read raced that callback. */
    uint32_t expected = before;
    (void)__atomic_compare_exchange_n(&associated_epoch, &expected, 0u, 0,
                                      __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    __atomic_store_n(&ready_epoch, 0u, __ATOMIC_RELEASE);
  }
  if (before != __atomic_load_n(&generation, __ATOMIC_ACQUIRE)) {
    rc = BK_ERR_BUSY;
    goto failure;
  }
  out->version = H2_BK_WIFI_IPV6_VERSION;
  out->size = sizeof(*out);
  return BK_OK;
failure:
  memset(out, 0, sizeof(*out));
  __atomic_store_n(&ready_epoch, 0u, __ATOMIC_RELEASE);
  return rc;
}

int h2_bk_wifi_ipv6_ready(void) {
  uint32_t before = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
  const uint32_t epoch = __atomic_load_n(&ready_epoch, __ATOMIC_ACQUIRE);
  return epoch != 0u && epoch == before &&
         epoch == __atomic_load_n(&associated_epoch, __ATOMIC_ACQUIRE) &&
         before == __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
}
