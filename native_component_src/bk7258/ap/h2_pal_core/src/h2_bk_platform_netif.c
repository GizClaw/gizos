#include "h2_bk_platform_core.h"
#include "h2_bk_netif_internal.h"

#include <components/log.h>
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/tcpip.h"
#include "net.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

#define H2_BK_NETIF_SNAPSHOT_MAX 16u
#define H2_BK_NETIF_REGISTRY_MAX 8u

typedef struct h2_bk_netif_registration {
    struct netif *netif;
    h2_pal_netif_kind_t kind;
} h2_bk_netif_registration_t;

typedef struct h2_bk_netif_snapshot {
    h2_pal_netif_status_t entries[H2_BK_NETIF_SNAPSHOT_MAX];
    size_t count;
    h2_pal_netif_ref_t default_ref;
    int default_valid;
} h2_bk_netif_snapshot_t;

typedef struct h2_bk_netif_snapshot_call {
    struct tcpip_api_call_data call;
    h2_bk_netif_snapshot_t *snapshot;
} h2_bk_netif_snapshot_call_t;

static h2_bk_netif_registration_t
    s_h2_bk_netif_registry[H2_BK_NETIF_REGISTRY_MAX];
static portMUX_TYPE s_h2_bk_netif_lock = portMUX_INITIALIZER_UNLOCKED;
static h2_pal_netif_ref_t s_h2_bk_default_ref;
static int s_h2_bk_default_known;
static int s_h2_bk_default_valid;

static h2_pal_netif_kind_t h2_bk_netif_kind(struct netif *netif) {
    h2_pal_netif_kind_t registered_kind = H2_PAL_NETIF_KIND_UNKNOWN;
    taskENTER_CRITICAL(&s_h2_bk_netif_lock);
    for (size_t i = 0u; i < H2_BK_NETIF_REGISTRY_MAX; ++i) {
        if (s_h2_bk_netif_registry[i].netif == netif) {
            registered_kind = s_h2_bk_netif_registry[i].kind;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_h2_bk_netif_lock);
    if (registered_kind != H2_PAL_NETIF_KIND_UNKNOWN) {
        return registered_kind;
    }
    if (netif == (struct netif *)net_get_sta_handle()) {
        return H2_PAL_NETIF_KIND_WIFI_STA;
    }
    if (netif == (struct netif *)net_get_uap_handle()) {
        return H2_PAL_NETIF_KIND_WIFI_AP;
    }
    if (netif->name[0] == 'p' && netif->name[1] == 'p') {
        return H2_PAL_NETIF_KIND_MODEM_DATA;
    }
    if (netif->name[0] == 'l' && netif->name[1] == 'o') {
        return H2_PAL_NETIF_KIND_LOOPBACK;
    }
    return H2_PAL_NETIF_KIND_UNKNOWN;
}

static void h2_bk_netif_set_addr(
    h2_pal_net_addr_t *out,
    const ip4_addr_t *address) {
    uint32_t value = lwip_ntohl(ip4_addr_get_u32(address));
    out->family = H2_PAL_NET_FAMILY_IPV4;
    out->ip[0] = (uint8_t)(value >> 24u);
    out->ip[1] = (uint8_t)(value >> 16u);
    out->ip[2] = (uint8_t)(value >> 8u);
    out->ip[3] = (uint8_t)value;
}

static h2_pal_netif_ref_t h2_bk_netif_ref(struct netif *netif) {
    h2_pal_netif_ref_t ref;
    memset(&ref, 0, sizeof(ref));
    ref.type = H2_PAL_NETIF_REF_ID;
    ref.kind = h2_bk_netif_kind(netif);
    ref.id = (uint32_t)netif_get_index(netif);
    return ref;
}

static void h2_bk_netif_fill_status(
    struct netif *netif,
    h2_pal_netif_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->ref = h2_bk_netif_ref(netif);
    out->kind = out->ref.kind;
    if (netif_is_up(netif)) {
        out->flags |= H2_PAL_NETIF_FLAG_UP;
    }
    if (netif_is_link_up(netif)) {
        out->flags |= H2_PAL_NETIF_FLAG_LINK_UP;
    }
    if (netif == netif_default) {
        out->flags |= H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
        for (size_t i = 0u;
             i < DNS_MAX_SERVERS && out->dns_count < H2_PAL_NETIF_DNS_MAX;
             ++i) {
            const ip_addr_t *dns = dns_getserver((u8_t)i);
            if (dns != NULL && IP_IS_V4(dns) &&
                !ip4_addr_isany_val(*ip_2_ip4(dns))) {
                h2_bk_netif_set_addr(
                    &out->dns[out->dns_count].addr, ip_2_ip4(dns));
                ++out->dns_count;
            }
#if LWIP_IPV6
            else if (dns != NULL && IP_IS_V6(dns) &&
                     !ip6_addr_isany(ip_2_ip6(dns))) {
              h2_pal_net_addr_t *address = &out->dns[out->dns_count++].addr;
              address->family = H2_PAL_NET_FAMILY_IPV6;
              memcpy(address->ip, ip_2_ip6(dns)->addr, 16u);
              if (ip6_addr_islinklocal(ip_2_ip6(dns)))
                address->scope_id = netif_get_index(netif);
            }
#endif
        }
    }
    if (!ip4_addr_isany_val(*netif_ip4_addr(netif))) {
        h2_bk_netif_set_addr(&out->ipv4, netif_ip4_addr(netif));
        h2_bk_netif_set_addr(&out->netmask4, netif_ip4_netmask(netif));
        h2_bk_netif_set_addr(&out->gateway4, netif_ip4_gw(netif));
        out->flags |= H2_PAL_NETIF_FLAG_HAS_IPV4;
    }
#if LWIP_IPV6
    for (unsigned i = 0u; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
      if (!ip6_addr_ispreferred(netif_ip6_addr_state(netif, i)))
        continue;
      const ip6_addr_t *address = netif_ip6_addr(netif, i);
      if (ip6_addr_isany(address))
        continue;
      out->ipv6.family = H2_PAL_NET_FAMILY_IPV6;
      memcpy(out->ipv6.ip, address->addr, 16u);
      out->ipv6.scope_id =
          ip6_addr_islinklocal(address) ? netif_get_index(netif) : 0u;
      out->flags |= H2_PAL_NETIF_FLAG_HAS_IPV6;
      if (h2_pal_net_ipv6_is_non_link_local_unicast(out->ipv6.ip))
        break;
    }
#endif
    out->mtu = netif->mtu;
    if (netif->hwaddr_len >= sizeof(out->mac)) {
        memcpy(out->mac, netif->hwaddr, sizeof(out->mac));
        out->mac_valid = 1u;
    }
}

static void h2_bk_netif_capture(h2_bk_netif_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    for (struct netif *netif = netif_list;
         netif != NULL && snapshot->count < H2_BK_NETIF_SNAPSHOT_MAX;
         netif = netif->next) {
        h2_bk_netif_fill_status(netif, &snapshot->entries[snapshot->count++]);
    }
    if (netif_default != NULL) {
        snapshot->default_ref = h2_bk_netif_ref(netif_default);
        snapshot->default_valid = 1;
    }
}

static err_t h2_bk_netif_snapshot_api_call(
    struct tcpip_api_call_data *data) {
    h2_bk_netif_snapshot_call_t *request =
        (h2_bk_netif_snapshot_call_t *)data;
    h2_bk_netif_capture(request->snapshot);
    return ERR_OK;
}

static h2_pal_result_t h2_bk_netif_snapshot(
    h2_bk_netif_snapshot_t *snapshot) {
    h2_bk_netif_snapshot_call_t request = {
        .snapshot = snapshot,
    };
    return tcpip_api_call(h2_bk_netif_snapshot_api_call, &request.call) == ERR_OK
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static void h2_bk_netif_name(
    const h2_pal_netif_status_t *status,
    char out_name[H2_PAL_NETIF_NAME_MAX]) {
    (void)snprintf(
        out_name,
        H2_PAL_NETIF_NAME_MAX,
        "%s%lu",
        status->kind == H2_PAL_NETIF_KIND_WIFI_STA ? "sta" :
        status->kind == H2_PAL_NETIF_KIND_WIFI_AP ? "ap" :
        status->kind == H2_PAL_NETIF_KIND_MODEM_DATA ? "ppp" :
        status->kind == H2_PAL_NETIF_KIND_LOOPBACK ? "lo" : "netif",
        (unsigned long)status->ref.id);
}

h2_pal_result_t h2_bk_netif_address_for_prefix(const char *prefix,
                                               h2_pal_net_family_t family,
                                               h2_pal_net_addr_t *out_addr) {
  if (out_addr == NULL ||
      (family != H2_PAL_NET_FAMILY_IPV4 && family != H2_PAL_NET_FAMILY_IPV6))
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_addr, 0, sizeof(*out_addr));
#if !LWIP_IPV6
  if (family == H2_PAL_NET_FAMILY_IPV6)
    return H2_PAL_ERR_UNSUPPORTED;
#endif
  h2_bk_netif_snapshot_t snapshot;
  h2_pal_result_t rc = h2_bk_netif_snapshot(&snapshot);
  if (rc != H2_PAL_OK)
    return rc;
  const int explicit_prefix = prefix != NULL && prefix[0] != '\0';
  const size_t prefix_len = explicit_prefix ? strlen(prefix) : 0u;
  int matched = !explicit_prefix;
  /* Actual default first, then physical interfaces, then loopback only for
   * an unfiltered host lookup. Explicit prefixes never change interfaces. */
  for (unsigned pass = 0u; pass < 3u; ++pass) {
    for (size_t index = 0u; index < snapshot.count; ++index) {
      const h2_pal_netif_status_t *status = &snapshot.entries[index];
      char name[H2_PAL_NETIF_NAME_MAX];
      h2_bk_netif_name(status, name);
      if (explicit_prefix && strncmp(name, prefix, prefix_len) != 0)
        continue;
      matched = 1;
      const int loopback = status->kind == H2_PAL_NETIF_KIND_LOOPBACK;
      if ((pass == 0u && !(status->flags & H2_PAL_NETIF_FLAG_DEFAULT_ROUTE)) ||
          (pass == 1u && loopback) ||
          (pass == 2u && (explicit_prefix || !loopback)) ||
          !(status->flags & H2_PAL_NETIF_FLAG_UP))
        continue;
      const uint32_t flag = family == H2_PAL_NET_FAMILY_IPV4
                                ? H2_PAL_NETIF_FLAG_HAS_IPV4
                                : H2_PAL_NETIF_FLAG_HAS_IPV6;
      if (!(status->flags & flag))
        continue;
      *out_addr =
          family == H2_PAL_NET_FAMILY_IPV4 ? status->ipv4 : status->ipv6;
      out_addr->port = 0u;
      return H2_PAL_OK;
    }
  }
  return matched ? H2_PAL_ERR_UNAVAILABLE : H2_PAL_ERR_NOT_FOUND;
}

typedef struct h2_bk_netif_name_call {
  struct tcpip_api_call_data call;
  uint32_t index;
  char name[H2_PAL_NETIF_NAME_MAX];
} h2_bk_netif_name_call_t;

static err_t h2_bk_netif_native_name_call(struct tcpip_api_call_data *raw) {
  h2_bk_netif_name_call_t *request = (h2_bk_netif_name_call_t *)raw;
  return netif_index_to_name((uint8_t)request->index, request->name) != NULL
             ? ERR_OK
             : ERR_IF;
}

h2_pal_result_t h2_bk_netif_native_name(uint32_t index, char *out,
                                        size_t size) {
  if (!index || index > UINT8_MAX || out == NULL ||
      size < H2_PAL_NETIF_NAME_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, size);
  h2_bk_netif_name_call_t request = {.index = index};
  if (tcpip_api_call(h2_bk_netif_native_name_call, &request.call) != ERR_OK)
    return H2_PAL_ERR_NOT_FOUND;
  memcpy(out, request.name, sizeof(request.name));
  return H2_PAL_OK;
}

static int h2_bk_netif_matches_filter(
    const h2_pal_netif_status_t *status,
    const h2_pal_netif_filter_t *filter) {
    if (filter == NULL) {
        return 1;
    }
    if (filter->kind != H2_PAL_NETIF_KIND_UNKNOWN &&
        filter->kind != status->kind) {
        return 0;
    }
    if (filter->id != 0u && filter->id != status->ref.id) {
        return 0;
    }
    if (filter->name != NULL && filter->name[0] != '\0') {
        char name[H2_PAL_NETIF_NAME_MAX];
        h2_bk_netif_name(status, name);
        return strncmp(filter->name, name, sizeof(name)) == 0;
    }
    return 1;
}

static int h2_bk_netif_matches_ref(
    const h2_pal_netif_status_t *status,
    const h2_pal_netif_ref_t *ref) {
    if (h2_pal_netif_ref_is_default(ref)) {
        return (status->flags & H2_PAL_NETIF_FLAG_DEFAULT_ROUTE) != 0u;
    }
    if (ref->type == H2_PAL_NETIF_REF_ID) {
        return ref->id == status->ref.id;
    }
    if (ref->type == H2_PAL_NETIF_REF_KIND) {
        return ref->kind == status->kind;
    }
    if (ref->type == H2_PAL_NETIF_REF_NAME) {
        char name[H2_PAL_NETIF_NAME_MAX];
        h2_bk_netif_name(status, name);
        return strncmp(ref->name, name, sizeof(name)) == 0;
    }
    return 0;
}

static h2_pal_result_t h2_bk_netif_list(
    void *user,
    const h2_pal_netif_filter_t *filter,
    h2_pal_netif_list_fn on_netif,
    void *callback_user) {
    (void)user;
    h2_bk_netif_snapshot_t snapshot;
    h2_pal_result_t rc = h2_bk_netif_snapshot(&snapshot);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (h2_bk_netif_matches_filter(&snapshot.entries[i], filter) &&
            on_netif(callback_user, &snapshot.entries[i].ref,
                     &snapshot.entries[i]) != 0) {
            return H2_PAL_EXIT;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk_netif_find(
    void *user,
    const h2_pal_netif_filter_t *filter,
    h2_pal_netif_ref_t *out_ref) {
    (void)user;
    h2_bk_netif_snapshot_t snapshot;
    h2_pal_result_t rc = h2_bk_netif_snapshot(&snapshot);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (h2_bk_netif_matches_filter(&snapshot.entries[i], filter)) {
            *out_ref = snapshot.entries[i].ref;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

typedef struct h2_bk_netif_status_call {
    struct tcpip_api_call_data call;
    const h2_pal_netif_ref_t *ref;
    h2_pal_netif_status_t *status;
    int found;
} h2_bk_netif_status_call_t;

static err_t h2_bk_netif_get_status_api_call(struct tcpip_api_call_data *raw) {
    h2_bk_netif_status_call_t *request = (h2_bk_netif_status_call_t *)raw;
    for (struct netif *netif = netif_list; netif != NULL; netif = netif->next) {
        h2_bk_netif_fill_status(netif, request->status);
        if (h2_bk_netif_matches_ref(request->status, request->ref)) {
            request->found = 1;
            return ERR_OK;
        }
    }
    memset(request->status, 0, sizeof(*request->status));
    return ERR_OK;
}

static h2_pal_result_t h2_bk_netif_get_status(
    void *user, const h2_pal_netif_ref_t *ref, h2_pal_netif_status_t *out_status) {
    (void)user;
    if (out_status == NULL) return H2_PAL_ERR_INVALID_ARG;
    memset(out_status, 0, sizeof(*out_status));
    /* A station address event runs on the SDK's small event-task stack. A
     * single-interface query must not reserve the full 16-interface list. */
    h2_bk_netif_status_call_t request = {.ref = ref, .status = out_status};
    if (tcpip_api_call(h2_bk_netif_get_status_api_call, &request.call) != ERR_OK) {
        memset(out_status, 0, sizeof(*out_status));
        return H2_PAL_ERR_IO;
    }
    return request.found ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t h2_bk_netif_get_dns_servers(
    void *user,
    const h2_pal_netif_ref_t *ref,
    h2_pal_netif_dns_server_t *out_servers,
    size_t max_servers,
    size_t *out_count) {
    (void)user;
    h2_bk_netif_snapshot_t snapshot;
    h2_pal_result_t rc = h2_bk_netif_snapshot(&snapshot);
    *out_count = 0u;
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (h2_bk_netif_matches_ref(&snapshot.entries[i], ref)) {
            size_t count = snapshot.entries[i].dns_count < max_servers
                ? snapshot.entries[i].dns_count
                : max_servers;
            if (count > 0u) {
                memcpy(out_servers, snapshot.entries[i].dns,
                       count * sizeof(out_servers[0]));
            }
            *out_count = count;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

typedef struct h2_bk_default_select_call {
    struct tcpip_api_call_data call;
    const h2_pal_netif_ref_t *ref;
    int found;
} h2_bk_default_select_call_t;

static err_t h2_bk_netif_set_default_api_call(
    struct tcpip_api_call_data *data) {
    h2_bk_default_select_call_t *request =
        (h2_bk_default_select_call_t *)data;
    for (struct netif *netif = netif_list; netif != NULL; netif = netif->next) {
        h2_pal_netif_status_t status;
        h2_bk_netif_fill_status(netif, &status);
        if (h2_bk_netif_matches_ref(&status, request->ref)) {
            netif_set_default(netif);
            request->found = 1;
            break;
        }
    }
    return ERR_OK;
}

static h2_pal_result_t h2_bk_netif_set_default(
    void *user,
    const h2_pal_netif_ref_t *ref) {
    (void)user;
    h2_bk_default_select_call_t request = {.ref = ref, .found = 0};
    if (tcpip_api_call(h2_bk_netif_set_default_api_call, &request.call) !=
        ERR_OK) {
        return H2_PAL_ERR_IO;
    }
    if (request.found == 0) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    return h2_bk_platform_netif_reconcile_default();
}

void h2_bk_platform_netif_register(
    void *netif_handle,
    h2_pal_netif_kind_t kind) {
    struct netif *netif = (struct netif *)netif_handle;
    if (netif == NULL || !h2_pal_netif_kind_is_valid(kind)) {
        return;
    }
    taskENTER_CRITICAL(&s_h2_bk_netif_lock);
    for (size_t i = 0u; i < H2_BK_NETIF_REGISTRY_MAX; ++i) {
        if (s_h2_bk_netif_registry[i].netif == netif ||
            s_h2_bk_netif_registry[i].netif == NULL) {
            s_h2_bk_netif_registry[i].netif = netif;
            s_h2_bk_netif_registry[i].kind = kind;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_h2_bk_netif_lock);
}

void h2_bk_platform_netif_unregister(void *netif_handle) {
    taskENTER_CRITICAL(&s_h2_bk_netif_lock);
    for (size_t i = 0u; i < H2_BK_NETIF_REGISTRY_MAX; ++i) {
        if (s_h2_bk_netif_registry[i].netif == netif_handle) {
            memset(&s_h2_bk_netif_registry[i], 0,
                   sizeof(s_h2_bk_netif_registry[i]));
            break;
        }
    }
    taskEXIT_CRITICAL(&s_h2_bk_netif_lock);
}

h2_pal_result_t h2_bk_platform_netif_monitor_init(void) {
    h2_bk_netif_snapshot_t snapshot;
    h2_pal_result_t rc = h2_bk_netif_snapshot(&snapshot);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    taskENTER_CRITICAL(&s_h2_bk_netif_lock);
    s_h2_bk_default_ref = snapshot.default_ref;
    s_h2_bk_default_valid = snapshot.default_valid;
    s_h2_bk_default_known = 1;
    taskEXIT_CRITICAL(&s_h2_bk_netif_lock);
    return H2_PAL_OK;
}

void h2_bk_platform_netif_monitor_deinit(void) {
    taskENTER_CRITICAL(&s_h2_bk_netif_lock);
    s_h2_bk_default_known = 0;
    s_h2_bk_default_valid = 0;
    memset(&s_h2_bk_default_ref, 0, sizeof(s_h2_bk_default_ref));
    taskEXIT_CRITICAL(&s_h2_bk_netif_lock);
}

h2_pal_result_t h2_bk_platform_netif_reconcile_default(void) {
    h2_bk_netif_snapshot_t snapshot;
    h2_pal_result_t rc = h2_bk_netif_snapshot(&snapshot);
    if (rc != H2_PAL_OK) {
        BK_LOGW("h2_netif", "default route query failed rc=%d\r\n", rc);
        return rc;
    }
    const h2_pal_netif_ref_t next = snapshot.default_ref;
    const int next_valid = snapshot.default_valid;

    h2_pal_netif_default_changed_t change;
    memset(&change, 0, sizeof(change));
    taskENTER_CRITICAL(&s_h2_bk_netif_lock);
    if (s_h2_bk_default_known == 0 ||
        (next_valid == s_h2_bk_default_valid &&
         (next_valid == 0 ||
          h2_pal_netif_ref_equal(&next, &s_h2_bk_default_ref)))) {
        taskEXIT_CRITICAL(&s_h2_bk_netif_lock);
        return H2_PAL_OK;
    }
    change.previous_valid = (uint8_t)s_h2_bk_default_valid;
    change.current_valid = (uint8_t)next_valid;
    if (s_h2_bk_default_valid != 0) {
        change.previous = s_h2_bk_default_ref;
    }
    if (next_valid != 0) {
        change.current = next;
    }
    s_h2_bk_default_ref = next;
    s_h2_bk_default_valid = next_valid;
    taskEXIT_CRITICAL(&s_h2_bk_netif_lock);

    h2_pal_system_event_t event = {
        .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
        .payload = &change,
        .payload_size = sizeof(change),
    };
    rc = h2_pal_system_event_post(
        h2_bk_platform_system_event_api(), &event, 0u);
    if (rc != H2_PAL_OK) {
        BK_LOGW("h2_netif", "default route event failed rc=%d\r\n", rc);
    }
    return rc;
}

const h2_pal_netif_api_t *h2_bk_platform_netif_api(void) {
    static const h2_pal_netif_vtable_t vtable = {
        .list = h2_bk_netif_list,
        .find = h2_bk_netif_find,
        .get_status = h2_bk_netif_get_status,
        .get_dns_servers = h2_bk_netif_get_dns_servers,
        .set_default = h2_bk_netif_set_default,
    };
    static const h2_pal_netif_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
