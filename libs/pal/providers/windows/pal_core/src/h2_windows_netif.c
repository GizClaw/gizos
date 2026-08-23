#include "h2_windows_internal.h"

#include <limits.h>
#include <string.h>

#define H2_WINDOWS_NETIF_CAPACITY 64u
#define H2_WINDOWS_NETIF_NAME_CAPACITY 256u

typedef struct windows_netif_entry {
    h2_pal_netif_status_t status;
    char name[H2_WINDOWS_NETIF_NAME_CAPACITY];
    uint32_t ipv4_id;
    uint32_t ipv6_id;
} windows_netif_entry_t;

typedef struct windows_netif_snapshot {
    windows_netif_entry_t entries[H2_WINDOWS_NETIF_CAPACITY];
    size_t count;
} windows_netif_snapshot_t;

static h2_pal_netif_kind_t windows_netif_kind(ULONG if_type) {
    switch (if_type) {
        case IF_TYPE_SOFTWARE_LOOPBACK:
            return H2_PAL_NETIF_KIND_LOOPBACK;
        case IF_TYPE_IEEE80211:
            return H2_PAL_NETIF_KIND_WIFI_STA;
        case IF_TYPE_ETHERNET_CSMACD:
            return H2_PAL_NETIF_KIND_ETHERNET;
        case IF_TYPE_PPP:
            return H2_PAL_NETIF_KIND_MODEM_DATA;
        default:
            return H2_PAL_NETIF_KIND_UNKNOWN;
    }
}

static void windows_netif_copy_address(h2_pal_net_addr_t *out,
                                       const SOCKADDR *address) {
    memset(out, 0, sizeof(*out));
    if (address->sa_family == AF_INET) {
        const SOCKADDR_IN *ipv4 = (const SOCKADDR_IN *)address;
        out->family = H2_PAL_NET_FAMILY_IPV4;
        memcpy(out->ip, &ipv4->sin_addr, 4u);
    } else if (address->sa_family == AF_INET6) {
        const SOCKADDR_IN6 *ipv6 = (const SOCKADDR_IN6 *)address;
        out->family = H2_PAL_NET_FAMILY_IPV6;
        memcpy(out->ip, &ipv6->sin6_addr, 16u);
    }
}

static void windows_netif_prefix_mask(uint8_t prefix_len,
                                      h2_pal_net_addr_t *out_mask) {
    memset(out_mask, 0, sizeof(*out_mask));
    out_mask->family = H2_PAL_NET_FAMILY_IPV4;
    for (size_t index = 0u; index < 4u; ++index) {
        if (prefix_len >= 8u) {
            out_mask->ip[index] = 0xffu;
            prefix_len = (uint8_t)(prefix_len - 8u);
        } else if (prefix_len != 0u) {
            out_mask->ip[index] = (uint8_t)(0xffu << (8u - prefix_len));
            prefix_len = 0u;
        }
    }
}

uint32_t h2_windows_netif_select_default(
    const h2_windows_route_candidate_t *candidates, size_t count) {
    uint32_t id = 0u;
    uint64_t best_metric = UINT64_MAX;
    for (size_t index = 0u; index < count; ++index) {
        const h2_windows_route_candidate_t *candidate = &candidates[index];
        if (!candidate->connected || candidate->interface_id == 0u) {
            continue;
        }
        uint64_t metric = (uint64_t)candidate->route_metric +
                          candidate->interface_metric;
        if (metric < best_metric ||
            (metric == best_metric &&
             (id == 0u || candidate->interface_id < id))) {
            best_metric = metric;
            id = candidate->interface_id;
        }
    }
    return id;
}

static uint32_t windows_netif_default_id(void) {
    MIB_IPFORWARD_TABLE2 *routes = NULL;
    if (GetIpForwardTable2(AF_INET, &routes) != NO_ERROR || routes == NULL) {
        return 0u;
    }
    h2_windows_route_candidate_t *candidates = h2_windows_heap_alloc(
        (size_t)routes->NumEntries * sizeof(*candidates));
    if (candidates == NULL && routes->NumEntries != 0u) {
        FreeMibTable(routes);
        return 0u;
    }
    size_t count = 0u;
    for (ULONG index = 0u; index < routes->NumEntries; ++index) {
        const MIB_IPFORWARD_ROW2 *route = &routes->Table[index];
        if (route->DestinationPrefix.Prefix.si_family != AF_INET ||
            route->DestinationPrefix.PrefixLength != 0u ||
            route->InterfaceIndex == 0u) {
            continue;
        }
        MIB_IPINTERFACE_ROW interface_row;
        InitializeIpInterfaceEntry(&interface_row);
        interface_row.Family = AF_INET;
        interface_row.InterfaceLuid = route->InterfaceLuid;
        interface_row.InterfaceIndex = route->InterfaceIndex;
        if (GetIpInterfaceEntry(&interface_row) != NO_ERROR) {
            continue;
        }
        candidates[count++] = (h2_windows_route_candidate_t){
            .interface_id = route->InterfaceIndex,
            .route_metric = route->Metric,
            .interface_metric = interface_row.Metric,
            .connected = interface_row.Connected,
        };
    }
    uint32_t id = h2_windows_netif_select_default(candidates, count);
    h2_windows_heap_free(candidates);
    FreeMibTable(routes);
    return id;
}

static h2_pal_result_t windows_netif_snapshot(
    windows_netif_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    ULONG size = 16u * 1024u;
    IP_ADAPTER_ADDRESSES *adapters = h2_windows_heap_alloc(size);
    if (adapters == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, adapters,
                                        &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        IP_ADAPTER_ADDRESSES *larger = h2_windows_heap_realloc(adapters, size);
        if (larger == NULL) {
            h2_windows_heap_free(adapters);
            return H2_PAL_ERR_NO_MEMORY;
        }
        adapters = larger;
        result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, adapters, &size);
    }
    if (result != NO_ERROR) {
        h2_windows_heap_free(adapters);
        return h2_windows_error_from_win32(result);
    }
    uint32_t default_id = windows_netif_default_id();
    for (IP_ADAPTER_ADDRESSES *adapter = adapters;
         adapter != NULL && snapshot->count < H2_WINDOWS_NETIF_CAPACITY;
         adapter = adapter->Next) {
        uint32_t id = adapter->IfIndex != 0u ? adapter->IfIndex
                                             : adapter->Ipv6IfIndex;
        if (id == 0u) {
            continue;
        }
        windows_netif_entry_t *entry = &snapshot->entries[snapshot->count++];
        h2_pal_netif_status_t *status = &entry->status;
        entry->ipv4_id = adapter->IfIndex;
        entry->ipv6_id = adapter->Ipv6IfIndex;
        status->ref.type = H2_PAL_NETIF_REF_ID;
        status->ref.id = id;
        status->ref.kind = windows_netif_kind(adapter->IfType);
        status->kind = status->ref.kind;
        if (adapter->OperStatus == IfOperStatusUp) {
            status->flags |= H2_PAL_NETIF_FLAG_UP |
                             H2_PAL_NETIF_FLAG_LINK_UP;
        }
        if (adapter->IfType == IF_TYPE_PPP) {
            status->flags |= H2_PAL_NETIF_FLAG_POINT_TO_POINT;
        }
        if (id == default_id) {
            status->flags |= H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
        }
        status->mtu = adapter->Mtu <= UINT16_MAX
                          ? (uint16_t)adapter->Mtu
                          : UINT16_MAX;
        if (adapter->PhysicalAddressLength == sizeof(status->mac)) {
            memcpy(status->mac, adapter->PhysicalAddress,
                   sizeof(status->mac));
            status->mac_valid = 1u;
        }
        if (adapter->FriendlyName != NULL) {
            char *name = h2_windows_wide_to_utf8(adapter->FriendlyName);
            if (name != NULL) {
                strncpy_s(entry->name, sizeof(entry->name), name, _TRUNCATE);
                h2_windows_heap_free(name);
            }
        }
        for (IP_ADAPTER_UNICAST_ADDRESS *address =
                 adapter->FirstUnicastAddress;
             address != NULL; address = address->Next) {
            if (address->Address.lpSockaddr->sa_family == AF_INET &&
                (status->flags & H2_PAL_NETIF_FLAG_HAS_IPV4) == 0u) {
                windows_netif_copy_address(&status->ipv4,
                                           address->Address.lpSockaddr);
                windows_netif_prefix_mask(address->OnLinkPrefixLength,
                                          &status->netmask4);
                status->flags |= H2_PAL_NETIF_FLAG_HAS_IPV4;
            } else if (address->Address.lpSockaddr->sa_family == AF_INET6 &&
                       (status->flags & H2_PAL_NETIF_FLAG_HAS_IPV6) == 0u) {
                windows_netif_copy_address(&status->ipv6,
                                           address->Address.lpSockaddr);
                status->flags |= H2_PAL_NETIF_FLAG_HAS_IPV6;
            }
        }
        for (IP_ADAPTER_GATEWAY_ADDRESS_LH *gateway =
                 adapter->FirstGatewayAddress;
             gateway != NULL; gateway = gateway->Next) {
            if (gateway->Address.lpSockaddr->sa_family == AF_INET) {
                windows_netif_copy_address(&status->gateway4,
                                           gateway->Address.lpSockaddr);
                break;
            }
        }
        for (IP_ADAPTER_DNS_SERVER_ADDRESS *dns =
                 adapter->FirstDnsServerAddress;
             dns != NULL && status->dns_count < H2_PAL_NETIF_DNS_MAX;
             dns = dns->Next) {
            if (dns->Address.lpSockaddr->sa_family != AF_INET &&
                dns->Address.lpSockaddr->sa_family != AF_INET6) {
                continue;
            }
            windows_netif_copy_address(
                &status->dns[status->dns_count++].addr,
                dns->Address.lpSockaddr);
        }
    }
    h2_windows_heap_free(adapters);
    return H2_PAL_OK;
}

static int windows_netif_filter_matches(
    const h2_pal_netif_filter_t *filter,
    const windows_netif_entry_t *entry) {
    return filter == NULL ||
           ((filter->kind == H2_PAL_NETIF_KIND_UNKNOWN ||
             filter->kind == entry->status.kind) &&
            (filter->name == NULL || filter->name[0] == '\0' ||
             _stricmp(filter->name, entry->name) == 0) &&
            (filter->id == 0u || filter->id == entry->status.ref.id));
}

static int windows_netif_ref_matches(const h2_pal_netif_ref_t *ref,
                                     const windows_netif_entry_t *entry) {
    if (ref->type == H2_PAL_NETIF_REF_ID) {
        return ref->id != 0u && ref->id == entry->status.ref.id;
    }
    if (ref->type == H2_PAL_NETIF_REF_NAME) {
        return memchr(ref->name, '\0', sizeof(ref->name)) != NULL &&
               _stricmp(ref->name, entry->name) == 0;
    }
    return ref->type == H2_PAL_NETIF_REF_KIND &&
           ref->kind == entry->status.kind;
}

static h2_pal_result_t windows_netif_list(
    void *user, const h2_pal_netif_filter_t *filter,
    h2_pal_netif_list_fn on_netif, void *callback_user) {
    (void)user;
    if (on_netif == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    windows_netif_snapshot_t snapshot;
    h2_pal_result_t result = windows_netif_snapshot(&snapshot);
    if (result != H2_PAL_OK) {
        return result;
    }
    for (size_t index = 0u; index < snapshot.count; ++index) {
        if (windows_netif_filter_matches(filter, &snapshot.entries[index]) &&
            on_netif(callback_user, &snapshot.entries[index].status.ref,
                     &snapshot.entries[index].status) != 0) {
            return H2_PAL_EXIT;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t windows_netif_find(
    void *user, const h2_pal_netif_filter_t *filter,
    h2_pal_netif_ref_t *out_ref) {
    (void)user;
    if (filter == NULL || out_ref == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    windows_netif_snapshot_t snapshot;
    h2_pal_result_t result = windows_netif_snapshot(&snapshot);
    if (result != H2_PAL_OK) {
        return result;
    }
    for (size_t index = 0u; index < snapshot.count; ++index) {
        if (windows_netif_filter_matches(filter, &snapshot.entries[index])) {
            *out_ref = snapshot.entries[index].status.ref;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t windows_netif_get_status(
    void *user, const h2_pal_netif_ref_t *ref,
    h2_pal_netif_status_t *out_status) {
    (void)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    windows_netif_snapshot_t snapshot;
    h2_pal_result_t result = windows_netif_snapshot(&snapshot);
    if (result != H2_PAL_OK) {
        return result;
    }
    for (size_t index = 0u; index < snapshot.count; ++index) {
        const h2_pal_netif_status_t *status =
            &snapshot.entries[index].status;
        if ((h2_pal_netif_ref_is_default(ref) &&
             (status->flags & H2_PAL_NETIF_FLAG_DEFAULT_ROUTE) != 0u) ||
            (ref != NULL &&
             windows_netif_ref_matches(ref, &snapshot.entries[index]))) {
            *out_status = *status;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t windows_netif_get_dns_servers(
    void *user, const h2_pal_netif_ref_t *ref,
    h2_pal_netif_dns_server_t *out_servers, size_t max_servers,
    size_t *out_count) {
    if (out_count == NULL || (out_servers == NULL && max_servers != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_count = 0u;
    h2_pal_netif_status_t status;
    h2_pal_result_t result = windows_netif_get_status(user, ref, &status);
    if (result != H2_PAL_OK) {
        return result;
    }
    size_t count = status.dns_count < max_servers ? status.dns_count
                                                   : max_servers;
    if (count != 0u) {
        memcpy(out_servers, status.dns, count * sizeof(out_servers[0]));
    }
    *out_count = count;
    return H2_PAL_OK;
}

h2_pal_result_t h2_windows_netif_default_ref(
    h2_windows_platform_t *platform, h2_pal_netif_ref_t *out_ref,
    int *out_valid) {
    if (platform == NULL || out_ref == NULL || out_valid == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_ref, 0, sizeof(*out_ref));
    *out_valid = 0;
    h2_pal_netif_status_t status;
    h2_pal_result_t result = windows_netif_get_status(platform, NULL, &status);
    if (result == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (result == H2_PAL_OK) {
        *out_ref = status.ref;
        *out_valid = 1;
    }
    return result;
}

h2_pal_result_t h2_windows_netif_resolve_ref_id(
    h2_windows_platform_t *platform, const h2_pal_netif_ref_t *ref,
    h2_pal_net_family_t family, uint32_t *out_id) {
    (void)platform;
    if (out_id == NULL ||
        (family != H2_PAL_NET_FAMILY_IPV4 &&
         family != H2_PAL_NET_FAMILY_IPV6)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    windows_netif_snapshot_t snapshot;
    h2_pal_result_t result = windows_netif_snapshot(&snapshot);
    if (result != H2_PAL_OK) {
        return result;
    }
    for (size_t index = 0u; index < snapshot.count; ++index) {
        const windows_netif_entry_t *entry = &snapshot.entries[index];
        if (!((h2_pal_netif_ref_is_default(ref) &&
               (entry->status.flags & H2_PAL_NETIF_FLAG_DEFAULT_ROUTE) != 0u) ||
              (ref != NULL && windows_netif_ref_matches(ref, entry)))) {
            continue;
        }
        const uint32_t id = family == H2_PAL_NET_FAMILY_IPV4
                                ? entry->ipv4_id
                                : entry->ipv6_id;
        const uint32_t address_flag = family == H2_PAL_NET_FAMILY_IPV4
                                          ? H2_PAL_NETIF_FLAG_HAS_IPV4
                                          : H2_PAL_NETIF_FLAG_HAS_IPV6;
        if (id == 0u || (entry->status.flags & address_flag) == 0u) {
            return H2_PAL_ERR_UNAVAILABLE;
        }
        *out_id = id;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_NOT_FOUND;
}

h2_pal_result_t h2_windows_netif_host_addr(
    h2_windows_platform_t *platform, const char *iface_prefix,
    h2_pal_net_addr_t *out_addr) {
    (void)platform;
    if (out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    windows_netif_snapshot_t snapshot;
    h2_pal_result_t result = windows_netif_snapshot(&snapshot);
    if (result != H2_PAL_OK) {
        return result;
    }
    size_t prefix_len = iface_prefix != NULL ? strlen(iface_prefix) : 0u;
    for (size_t index = 0u; index < snapshot.count; ++index) {
        const windows_netif_entry_t *entry = &snapshot.entries[index];
        if ((entry->status.flags & H2_PAL_NETIF_FLAG_HAS_IPV4) != 0u &&
            (prefix_len == 0u ||
             _strnicmp(entry->name, iface_prefix, prefix_len) == 0)) {
            *out_addr = entry->status.ipv4;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

const h2_pal_netif_vtable_t h2_windows_netif_vtable = {
    .list = windows_netif_list,
    .find = windows_netif_find,
    .get_status = windows_netif_get_status,
    .get_dns_servers = windows_netif_get_dns_servers,
};
