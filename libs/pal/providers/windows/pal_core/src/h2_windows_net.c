#include "h2_windows_internal.h"

#include <limits.h>
#include <string.h>

WINSOCK_API_LINKAGE int WSAAPI GetAddrInfoExCancel(LPHANDLE handle);

struct h2_pal_net_resolver {
    h2_windows_platform_t *platform;
    CRITICAL_SECTION lock;
    HANDLE completed_event;
    OVERLAPPED overlapped;
    HANDLE cancel_handle;
    ADDRINFOEXW hints;
    ADDRINFOEXW *addresses;
    wchar_t *host;
    size_t registry_index;
    h2_pal_net_addr_t addr;
    h2_pal_result_t result;
    LONG references;
    int completed;
    int closed;
};

static int windows_family(h2_pal_net_family_t family) {
    if (family == H2_PAL_NET_FAMILY_IPV4) {
        return AF_INET;
    }
    if (family == H2_PAL_NET_FAMILY_IPV6) {
        return AF_INET6;
    }
    return AF_UNSPEC;
}

static int windows_addr_to_sockaddr(const h2_pal_net_addr_t *addr,
                                    SOCKADDR_STORAGE *storage,
                                    int *out_len) {
    if (addr == NULL || storage == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(storage, 0, sizeof(*storage));
    if (addr->family == H2_PAL_NET_FAMILY_IPV4) {
        SOCKADDR_IN *ipv4 = (SOCKADDR_IN *)storage;
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(addr->port);
        memcpy(&ipv4->sin_addr, addr->ip, 4u);
        *out_len = (int)sizeof(*ipv4);
        return H2_PAL_OK;
    }
    if (addr->family == H2_PAL_NET_FAMILY_IPV6) {
        SOCKADDR_IN6 *ipv6 = (SOCKADDR_IN6 *)storage;
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(addr->port);
        memcpy(&ipv6->sin6_addr, addr->ip, 16u);
        *out_len = (int)sizeof(*ipv6);
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_INVALID_ARG;
}

static int windows_sockaddr_to_addr(const SOCKADDR *address,
                                    h2_pal_net_addr_t *out_addr) {
    if (address == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_addr, 0, sizeof(*out_addr));
    if (address->sa_family == AF_INET) {
        const SOCKADDR_IN *ipv4 = (const SOCKADDR_IN *)address;
        out_addr->family = H2_PAL_NET_FAMILY_IPV4;
        out_addr->port = ntohs(ipv4->sin_port);
        memcpy(out_addr->ip, &ipv4->sin_addr, 4u);
        return H2_PAL_OK;
    }
    if (address->sa_family == AF_INET6) {
        const SOCKADDR_IN6 *ipv6 = (const SOCKADDR_IN6 *)address;
        out_addr->family = H2_PAL_NET_FAMILY_IPV6;
        out_addr->port = ntohs(ipv6->sin6_port);
        memcpy(out_addr->ip, &ipv6->sin6_addr, 16u);
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t windows_resolve_wide(const wchar_t *host,
                                            h2_pal_net_addr_t *out_addr) {
    ADDRINFOEXW hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOEXW *addresses = NULL;
    int error = GetAddrInfoExW(host, NULL, NS_ALL, NULL, &hints, &addresses,
                               NULL, NULL, NULL, NULL);
    h2_pal_result_t result = error == 0 ? H2_PAL_ERR_NOT_FOUND
                                        : h2_windows_error_from_wsa(error);
    for (ADDRINFOEXW *entry = addresses; entry != NULL;
         entry = entry->ai_next) {
        if (entry->ai_family == AF_INET || entry->ai_family == AF_INET6) {
            result = windows_sockaddr_to_addr(entry->ai_addr, out_addr);
            break;
        }
    }
    if (addresses != NULL) {
        FreeAddrInfoExW(addresses);
    }
    return result;
}

static int windows_net_resolve_addr(void *user, const char *host,
                                    h2_pal_net_addr_t *out_addr) {
    (void)user;
    if (host == NULL || host[0] == '\0' || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    wchar_t *wide = h2_windows_utf8_to_wide(host);
    if (wide == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t result = windows_resolve_wide(wide, out_addr);
    h2_windows_heap_free(wide);
    return result;
}

static void windows_resolver_destroy(h2_pal_net_resolver_t *resolver) {
    h2_windows_platform_t *platform = resolver->platform;
    EnterCriticalSection(&platform->lock);
    if (resolver->registry_index < H2_WINDOWS_RESOLVER_CAPACITY &&
        platform->resolvers[resolver->registry_index] == resolver) {
        platform->resolvers[resolver->registry_index] = NULL;
        --platform->resolver_count;
    }
    WakeAllConditionVariable(&platform->idle);
    LeaveCriticalSection(&platform->lock);
    if (resolver->addresses != NULL) {
        FreeAddrInfoExW(resolver->addresses);
    }
    (void)CloseHandle(resolver->completed_event);
    DeleteCriticalSection(&resolver->lock);
    h2_windows_heap_free(resolver->host);
    h2_windows_heap_free(resolver);
}

static void windows_resolver_release(h2_pal_net_resolver_t *resolver) {
    if (InterlockedDecrement(&resolver->references) == 0) {
        windows_resolver_destroy(resolver);
    }
}

static void windows_resolver_complete(h2_pal_net_resolver_t *resolver,
                                      int native_result) {
    h2_pal_net_addr_t addr = {0};
    h2_pal_result_t result = native_result == 0
                                 ? H2_PAL_ERR_NOT_FOUND
                                 : h2_windows_error_from_wsa(native_result);
    if (native_result == 0) {
        for (ADDRINFOEXW *entry = resolver->addresses; entry != NULL;
             entry = entry->ai_next) {
            if (entry->ai_family == AF_INET || entry->ai_family == AF_INET6) {
                result = windows_sockaddr_to_addr(entry->ai_addr, &addr);
                break;
            }
        }
    }
    EnterCriticalSection(&resolver->lock);
    resolver->result = result;
    if (result == H2_PAL_OK) {
        resolver->addr = addr;
    }
    resolver->completed = 1;
    (void)SetEvent(resolver->completed_event);
    LeaveCriticalSection(&resolver->lock);
    windows_resolver_release(resolver);
}

static VOID CALLBACK windows_resolver_callback(
    DWORD error, DWORD bytes, LPWSAOVERLAPPED overlapped) {
    (void)bytes;
    h2_pal_net_resolver_t *resolver = CONTAINING_RECORD(
        overlapped, h2_pal_net_resolver_t, overlapped);
    windows_resolver_complete(resolver, (int)error);
}

static h2_pal_result_t windows_net_resolve_start(
    void *user, const char *host, h2_pal_net_resolver_t **out_resolver) {
    h2_windows_platform_t *platform = user;
    if (host == NULL || host[0] == '\0' || out_resolver == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_resolver = NULL;
    h2_pal_net_resolver_t *resolver =
        h2_windows_heap_alloc(sizeof(*resolver));
    if (resolver == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(resolver, 0, sizeof(*resolver));
    resolver->platform = platform;
    resolver->references = 2;
    resolver->host = h2_windows_utf8_to_wide(host);
    resolver->completed_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&resolver->lock);
    if (resolver->host == NULL || resolver->completed_event == NULL) {
        if (resolver->completed_event != NULL) {
            (void)CloseHandle(resolver->completed_event);
        }
        DeleteCriticalSection(&resolver->lock);
        h2_windows_heap_free(resolver->host);
        h2_windows_heap_free(resolver);
        return H2_PAL_ERR_NO_MEMORY;
    }
    resolver->hints.ai_family = AF_UNSPEC;
    resolver->hints.ai_socktype = SOCK_STREAM;
    resolver->registry_index = H2_WINDOWS_RESOLVER_CAPACITY;
    EnterCriticalSection(&platform->lock);
    if (InterlockedCompareExchange(&platform->shutting_down, 0, 0) != 0) {
        LeaveCriticalSection(&platform->lock);
        (void)CloseHandle(resolver->completed_event);
        DeleteCriticalSection(&resolver->lock);
        h2_windows_heap_free(resolver->host);
        h2_windows_heap_free(resolver);
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (platform->resolver_count >= H2_WINDOWS_RESOLVER_CAPACITY) {
        LeaveCriticalSection(&platform->lock);
        (void)CloseHandle(resolver->completed_event);
        DeleteCriticalSection(&resolver->lock);
        h2_windows_heap_free(resolver->host);
        h2_windows_heap_free(resolver);
        return H2_PAL_ERR_NO_SPACE;
    }
    for (size_t index = 0u; index < H2_WINDOWS_RESOLVER_CAPACITY; ++index) {
        if (platform->resolvers[index] == NULL) {
            platform->resolvers[index] = resolver;
            resolver->registry_index = index;
            ++platform->resolver_count;
            break;
        }
    }
    if (resolver->registry_index == H2_WINDOWS_RESOLVER_CAPACITY) {
        LeaveCriticalSection(&platform->lock);
        (void)CloseHandle(resolver->completed_event);
        DeleteCriticalSection(&resolver->lock);
        h2_windows_heap_free(resolver->host);
        h2_windows_heap_free(resolver);
        return H2_PAL_ERR_NO_SPACE;
    }
    h2_windows_object_acquire(platform);
    LeaveCriticalSection(&platform->lock);
    int native_result = GetAddrInfoExW(
        resolver->host, NULL, NS_ALL, NULL, &resolver->hints,
        &resolver->addresses, NULL, &resolver->overlapped,
        windows_resolver_callback, &resolver->cancel_handle);
    if (native_result != 0 && native_result != WSA_IO_PENDING) {
        h2_windows_object_release(platform);
        windows_resolver_release(resolver);
        windows_resolver_release(resolver);
        return h2_windows_error_from_wsa(native_result);
    }
    if (native_result == 0) {
        windows_resolver_complete(resolver, 0);
    }
    *out_resolver = resolver;
    return H2_PAL_OK;
}

static h2_pal_result_t windows_net_resolve_poll(
    void *user, h2_pal_net_resolver_t *resolver, h2_pal_net_addr_t *out_addr,
    uint32_t timeout_ms) {
    if (resolver == NULL || resolver->platform != user || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    DWORD wait = WaitForSingleObject(resolver->completed_event, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                : H2_PAL_ERR_TIMEOUT;
    }
    if (wait != WAIT_OBJECT_0) {
        return H2_PAL_ERR_IO;
    }
    EnterCriticalSection(&resolver->lock);
    h2_pal_result_t result = resolver->closed ? H2_PAL_ERR_INVALID_STATE
                                               : resolver->result;
    if (result == H2_PAL_OK) {
        *out_addr = resolver->addr;
    }
    LeaveCriticalSection(&resolver->lock);
    return result;
}

static void windows_net_resolve_close(void *user,
                                      h2_pal_net_resolver_t *resolver) {
    if (resolver == NULL || resolver->platform != user) {
        return;
    }
    EnterCriticalSection(&resolver->lock);
    if (resolver->closed) {
        LeaveCriticalSection(&resolver->lock);
        return;
    }
    resolver->closed = 1;
    int completed = resolver->completed;
    HANDLE cancel_handle = resolver->cancel_handle;
    h2_windows_platform_t *platform = resolver->platform;
    LeaveCriticalSection(&resolver->lock);
    if (!completed && cancel_handle != NULL) {
        (void)GetAddrInfoExCancel(&resolver->cancel_handle);
    }
    h2_windows_object_release(platform);
    windows_resolver_release(resolver);
}

static int windows_net_wait(SOCKET socket_value, int write,
                            uint32_t timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket_value, &set);
    TIMEVAL timeout;
    timeout.tv_sec = (long)(timeout_ms / 1000u);
    timeout.tv_usec = (long)(timeout_ms % 1000u) * 1000L;
    int result = select(0, write ? NULL : &set, write ? &set : NULL, NULL,
                        timeout_ms == UINT32_MAX ? NULL : &timeout);
    if (result > 0) {
        return H2_PAL_OK;
    }
    if (result == 0) {
        return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                : H2_PAL_ERR_TIMEOUT;
    }
    return h2_windows_error_from_wsa(WSAGetLastError());
}

static h2_windows_socket_slot_t *windows_net_slot_locked(
    h2_windows_platform_t *platform, h2_pal_net_socket_t token,
    size_t *out_index) {
    if (token < (int)H2_WINDOWS_SOCKET_CAPACITY) {
        return NULL;
    }
    size_t index = (size_t)token % H2_WINDOWS_SOCKET_CAPACITY;
    uint32_t generation = (uint32_t)token / H2_WINDOWS_SOCKET_CAPACITY;
    h2_windows_socket_slot_t *slot = &platform->sockets[index];
    if (!slot->in_use || slot->generation != generation) {
        return NULL;
    }
    if (out_index != NULL) {
        *out_index = index;
    }
    return slot;
}

h2_windows_socket_slot_t *h2_windows_net_lock_slot(
    h2_windows_platform_t *platform, h2_pal_net_socket_t token) {
    EnterCriticalSection(&platform->lock);
    h2_windows_socket_slot_t *slot =
        windows_net_slot_locked(platform, token, NULL);
    if (slot != NULL) {
        EnterCriticalSection(&slot->lock);
    }
    LeaveCriticalSection(&platform->lock);
    return slot;
}

void h2_windows_net_unlock_slot(h2_windows_socket_slot_t *slot) {
    LeaveCriticalSection(&slot->lock);
}

static h2_pal_result_t windows_net_store_socket(
    h2_windows_platform_t *platform, SOCKET socket_value,
    h2_pal_net_socket_t *out_token) {
    EnterCriticalSection(&platform->lock);
    if (InterlockedCompareExchange(&platform->shutting_down, 0, 0) != 0) {
        LeaveCriticalSection(&platform->lock);
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t index = 0u; index < H2_WINDOWS_SOCKET_CAPACITY; ++index) {
        h2_windows_socket_slot_t *slot = &platform->sockets[index];
        if (slot->in_use || slot->closing) {
            continue;
        }
        uint32_t max_generation =
            (uint32_t)INT_MAX / H2_WINDOWS_SOCKET_CAPACITY;
        if (slot->generation >= max_generation) {
            continue;
        }
        ++slot->generation;
        slot->socket = socket_value;
        slot->in_use = 1;
        slot->connecting = 0;
        slot->tls = NULL;
        slot->tls_context = NULL;
        *out_token = (h2_pal_net_socket_t)(
            slot->generation * H2_WINDOWS_SOCKET_CAPACITY + index);
        h2_windows_object_acquire(platform);
        LeaveCriticalSection(&platform->lock);
        return H2_PAL_OK;
    }
    LeaveCriticalSection(&platform->lock);
    return H2_PAL_ERR_NO_SPACE;
}

static int windows_net_bind_socket(h2_windows_platform_t *platform,
                                   SOCKET socket_value,
                                   h2_pal_net_family_t family, uint16_t port,
                                   const h2_pal_net_bind_t *binding) {
    h2_pal_net_addr_t source;
    memset(&source, 0, sizeof(source));
    source.family = family;
    source.port = port;
    if (binding != NULL && binding->type == H2_PAL_NET_BIND_SOURCE_ADDR) {
        if (binding->source_addr.family != family) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        source = binding->source_addr;
        source.port = port;
    } else if (binding != NULL && binding->type == H2_PAL_NET_BIND_NETIF) {
        uint32_t id = 0u;
        h2_pal_result_t result = h2_windows_netif_resolve_ref_id(
            platform, binding->netif, family, &id);
        if (result != H2_PAL_OK) {
            return result;
        }
        if (family == H2_PAL_NET_FAMILY_IPV4) {
            DWORD value = htonl(id);
            if (setsockopt(socket_value, IPPROTO_IP, IP_UNICAST_IF,
                           (const char *)&value, sizeof(value)) != 0) {
                return h2_windows_error_from_wsa(WSAGetLastError());
            }
        } else {
            DWORD value = id;
            if (setsockopt(socket_value, IPPROTO_IPV6, IPV6_UNICAST_IF,
                           (const char *)&value, sizeof(value)) != 0) {
                return h2_windows_error_from_wsa(WSAGetLastError());
            }
        }
    } else if (binding != NULL &&
               binding->type != H2_PAL_NET_BIND_DEFAULT) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    SOCKADDR_STORAGE address;
    int address_len = 0;
    int result = windows_addr_to_sockaddr(&source, &address, &address_len);
    if (result != H2_PAL_OK ||
        bind(socket_value, (SOCKADDR *)&address, address_len) != 0) {
        return result != H2_PAL_OK
                   ? result
                   : h2_windows_error_from_wsa(WSAGetLastError());
    }
    return H2_PAL_OK;
}

static int windows_net_open(h2_windows_platform_t *platform,
                            h2_pal_net_family_t family, int socket_type,
                            h2_pal_net_socket_t *out_socket) {
    if (out_socket == NULL || windows_family(family) == AF_UNSPEC) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_socket = -1;
    SOCKET native = socket(windows_family(family), socket_type,
                           socket_type == SOCK_STREAM ? IPPROTO_TCP
                                                      : IPPROTO_UDP);
    if (native == INVALID_SOCKET) {
        return h2_windows_error_from_wsa(WSAGetLastError());
    }
    u_long nonblocking = 1u;
    if (ioctlsocket(native, FIONBIO, &nonblocking) != 0) {
        (void)closesocket(native);
        return h2_windows_error_from_wsa(WSAGetLastError());
    }
    int result = windows_net_store_socket(platform, native, out_socket);
    if (result != H2_PAL_OK) {
        (void)closesocket(native);
    }
    return result;
}

static int windows_net_udp_open_bound(void *user, h2_pal_net_family_t family,
                                      uint16_t port,
                                      const h2_pal_net_bind_t *binding,
                                      h2_pal_net_socket_t *out_socket,
                                      h2_pal_net_addr_t *out_bind_addr) {
    h2_windows_platform_t *platform = user;
    if (out_socket == NULL || out_bind_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int result = windows_net_open(platform, family, SOCK_DGRAM, out_socket);
    if (result != H2_PAL_OK) {
        return result;
    }
    h2_windows_socket_slot_t *slot =
        h2_windows_net_lock_slot(platform, *out_socket);
    int reuse = 1;
    (void)setsockopt(slot->socket, SOL_SOCKET, SO_REUSEADDR,
                     (const char *)&reuse, sizeof(reuse));
    result = windows_net_bind_socket(platform, slot->socket, family, port,
                                     binding);
    if (result == H2_PAL_OK) {
        SOCKADDR_STORAGE address;
        int address_len = (int)sizeof(address);
        if (getsockname(slot->socket, (SOCKADDR *)&address, &address_len) != 0) {
            result = h2_windows_error_from_wsa(WSAGetLastError());
        } else {
            result = windows_sockaddr_to_addr((SOCKADDR *)&address,
                                              out_bind_addr);
        }
    }
    h2_windows_net_unlock_slot(slot);
    if (result != H2_PAL_OK) {
        h2_pal_net_socket_t token = *out_socket;
        *out_socket = -1;
        h2_windows_net_vtable.close(platform, token);
    }
    return result;
}

static int windows_net_udp_open(void *user, h2_pal_net_family_t family,
                                uint16_t port,
                                h2_pal_net_socket_t *out_socket,
                                h2_pal_net_addr_t *out_bind_addr) {
    return windows_net_udp_open_bound(user, family, port, NULL, out_socket,
                                      out_bind_addr);
}

static int windows_net_udp_sendto(void *user, h2_pal_net_socket_t token,
                                  const h2_pal_net_addr_t *addr,
                                  const uint8_t *data, size_t len) {
    h2_windows_platform_t *platform = user;
    if (addr == NULL || (data == NULL && len != 0u) || len > INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    SOCKADDR_STORAGE address;
    int address_len = 0;
    int result = windows_addr_to_sockaddr(addr, &address, &address_len);
    if (result != H2_PAL_OK) {
        return result;
    }
    h2_windows_socket_slot_t *slot = h2_windows_net_lock_slot(platform, token);
    if (slot == NULL || slot->tls != NULL) {
        if (slot != NULL) h2_windows_net_unlock_slot(slot);
        return H2_PAL_ERR_INVALID_ARG;
    }
    int sent = sendto(slot->socket, (const char *)data, (int)len, 0,
                      (SOCKADDR *)&address, address_len);
    result = sent == SOCKET_ERROR ? h2_windows_error_from_wsa(WSAGetLastError())
                                  : sent;
    h2_windows_net_unlock_slot(slot);
    return result;
}

static int windows_net_udp_recvfrom(void *user, h2_pal_net_socket_t token,
                                    h2_pal_net_addr_t *out_addr, uint8_t *data,
                                    size_t len, uint32_t timeout_ms) {
    h2_windows_platform_t *platform = user;
    if (out_addr == NULL || (data == NULL && len != 0u) || len > INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_windows_socket_slot_t *slot = h2_windows_net_lock_slot(platform, token);
    if (slot == NULL || slot->tls != NULL) {
        if (slot != NULL) h2_windows_net_unlock_slot(slot);
        return H2_PAL_ERR_INVALID_ARG;
    }
    int result = windows_net_wait(slot->socket, 0, timeout_ms);
    if (result == H2_PAL_OK) {
        SOCKADDR_STORAGE address;
        int address_len = (int)sizeof(address);
        int received = recvfrom(slot->socket, (char *)data, (int)len, 0,
                                (SOCKADDR *)&address, &address_len);
        result = received == SOCKET_ERROR
                     ? h2_windows_error_from_wsa(WSAGetLastError())
                     : received;
        if (received >= 0) {
            (void)windows_sockaddr_to_addr((SOCKADDR *)&address, out_addr);
        }
    }
    h2_windows_net_unlock_slot(slot);
    return result;
}

static int windows_net_udp_join_multicast(void *user,
                                           h2_pal_net_socket_t token,
                                           const h2_pal_net_addr_t *addr) {
    h2_windows_platform_t *platform = user;
    if (addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_windows_socket_slot_t *slot = h2_windows_net_lock_slot(platform, token);
    if (slot == NULL || slot->tls != NULL) {
        if (slot != NULL) h2_windows_net_unlock_slot(slot);
        return H2_PAL_ERR_INVALID_ARG;
    }
    int native_result;
    if (addr->family == H2_PAL_NET_FAMILY_IPV4) {
        struct ip_mreq request;
        memset(&request, 0, sizeof(request));
        memcpy(&request.imr_multiaddr, addr->ip, 4u);
        request.imr_interface.s_addr = INADDR_ANY;
        native_result = setsockopt(slot->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                   (const char *)&request, sizeof(request));
    } else if (addr->family == H2_PAL_NET_FAMILY_IPV6) {
        struct ipv6_mreq request;
        memset(&request, 0, sizeof(request));
        memcpy(&request.ipv6mr_multiaddr, addr->ip, 16u);
        native_result = setsockopt(slot->socket, IPPROTO_IPV6,
                                   IPV6_ADD_MEMBERSHIP,
                                   (const char *)&request, sizeof(request));
    } else {
        h2_windows_net_unlock_slot(slot);
        return H2_PAL_ERR_INVALID_ARG;
    }
    int result = native_result == 0
                     ? H2_PAL_OK
                     : h2_windows_error_from_wsa(WSAGetLastError());
    h2_windows_net_unlock_slot(slot);
    return result;
}

static int windows_net_tcp_open(void *user, h2_pal_net_family_t family,
                                h2_pal_net_socket_t *out_socket) {
    return windows_net_open(user, family, SOCK_STREAM, out_socket);
}

static int windows_net_tcp_open_bound(void *user,
                                      h2_pal_net_family_t family,
                                      const h2_pal_net_bind_t *binding,
                                      h2_pal_net_socket_t *out_socket) {
    h2_windows_platform_t *platform = user;
    int result = windows_net_open(platform, family, SOCK_STREAM, out_socket);
    if (result != H2_PAL_OK || binding == NULL ||
        binding->type == H2_PAL_NET_BIND_DEFAULT) {
        return result;
    }
    h2_windows_socket_slot_t *slot =
        h2_windows_net_lock_slot(platform, *out_socket);
    result = windows_net_bind_socket(platform, slot->socket, family, 0u,
                                     binding);
    h2_windows_net_unlock_slot(slot);
    if (result != H2_PAL_OK) {
        h2_pal_net_socket_t token = *out_socket;
        *out_socket = -1;
        h2_windows_net_vtable.close(platform, token);
    }
    return result;
}

static h2_pal_result_t windows_net_tcp_connect(
    void *user, h2_pal_net_socket_t token, const h2_pal_net_addr_t *addr,
    uint32_t timeout_ms) {
    h2_windows_platform_t *platform = user;
    SOCKADDR_STORAGE address;
    int address_len = 0;
    int result = windows_addr_to_sockaddr(addr, &address, &address_len);
    if (result != H2_PAL_OK) {
        return result;
    }
    h2_windows_socket_slot_t *slot = h2_windows_net_lock_slot(platform, token);
    if (slot == NULL || slot->tls != NULL) {
        if (slot != NULL) h2_windows_net_unlock_slot(slot);
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (!slot->connecting) {
        if (connect(slot->socket, (SOCKADDR *)&address, address_len) == 0) {
            h2_windows_net_unlock_slot(slot);
            return H2_PAL_OK;
        }
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS &&
            error != WSAEALREADY) {
            h2_windows_net_unlock_slot(slot);
            return h2_windows_error_from_wsa(error);
        }
        slot->connecting = 1;
        slot->connect_addr = *addr;
    } else if (memcmp(&slot->connect_addr, addr, sizeof(*addr)) != 0) {
        h2_windows_net_unlock_slot(slot);
        return H2_PAL_ERR_INVALID_STATE;
    }
    result = windows_net_wait(slot->socket, 1, timeout_ms);
    if (result == H2_PAL_OK) {
        int socket_error = 0;
        int error_len = (int)sizeof(socket_error);
        if (getsockopt(slot->socket, SOL_SOCKET, SO_ERROR,
                       (char *)&socket_error, &error_len) != 0) {
            result = h2_windows_error_from_wsa(WSAGetLastError());
        } else if (socket_error != 0) {
            result = h2_windows_error_from_wsa(socket_error);
        } else {
            slot->connecting = 0;
        }
    }
    h2_windows_net_unlock_slot(slot);
    return result;
}

static int windows_net_tcp_listen(void *user, h2_pal_net_family_t family,
                                  uint16_t port,
                                  const h2_pal_net_bind_t *binding,
                                  h2_pal_net_socket_t *out_socket,
                                  h2_pal_net_addr_t *out_bind_addr) {
    h2_windows_platform_t *platform = user;
    if (out_socket == NULL || out_bind_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int result = windows_net_open(platform, family, SOCK_STREAM, out_socket);
    if (result != H2_PAL_OK) {
        return result;
    }
    h2_windows_socket_slot_t *slot =
        h2_windows_net_lock_slot(platform, *out_socket);
    int reuse = 1;
    (void)setsockopt(slot->socket, SOL_SOCKET, SO_REUSEADDR,
                     (const char *)&reuse, sizeof(reuse));
    result = windows_net_bind_socket(platform, slot->socket, family, port,
                                     binding);
    if (result == H2_PAL_OK && listen(slot->socket, SOMAXCONN) != 0) {
        result = h2_windows_error_from_wsa(WSAGetLastError());
    }
    if (result == H2_PAL_OK) {
        SOCKADDR_STORAGE address;
        int address_len = (int)sizeof(address);
        if (getsockname(slot->socket, (SOCKADDR *)&address, &address_len) != 0) {
            result = h2_windows_error_from_wsa(WSAGetLastError());
        } else {
            result = windows_sockaddr_to_addr((SOCKADDR *)&address,
                                              out_bind_addr);
        }
    }
    h2_windows_net_unlock_slot(slot);
    if (result != H2_PAL_OK) {
        h2_pal_net_socket_t token = *out_socket;
        *out_socket = -1;
        h2_windows_net_vtable.close(platform, token);
    }
    return result;
}

static h2_pal_result_t windows_net_tcp_accept(void *user,
                                              h2_pal_net_socket_t token,
                                              h2_pal_net_socket_t *out_socket,
                                              h2_pal_net_addr_t *out_peer_addr,
                                              uint32_t timeout_ms) {
    h2_windows_platform_t *platform = user;
    if (out_socket == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_socket = -1;
    h2_windows_socket_slot_t *slot = h2_windows_net_lock_slot(platform, token);
    if (slot == NULL || slot->tls != NULL) {
        if (slot != NULL) h2_windows_net_unlock_slot(slot);
        return H2_PAL_ERR_INVALID_ARG;
    }
    int result = windows_net_wait(slot->socket, 0, timeout_ms);
    if (result != H2_PAL_OK) {
        h2_windows_net_unlock_slot(slot);
        return result;
    }
    SOCKADDR_STORAGE address;
    int address_len = (int)sizeof(address);
    SOCKET accepted = accept(slot->socket, (SOCKADDR *)&address, &address_len);
    h2_windows_net_unlock_slot(slot);
    if (accepted == INVALID_SOCKET) {
        int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAECONNRESET
                   ? H2_PAL_ERR_WOULD_BLOCK
                   : h2_windows_error_from_wsa(error);
    }
    u_long nonblocking = 1u;
    if (ioctlsocket(accepted, FIONBIO, &nonblocking) != 0) {
        int error = WSAGetLastError();
        (void)closesocket(accepted);
        return h2_windows_error_from_wsa(error);
    }
    result = windows_net_store_socket(platform, accepted, out_socket);
    if (result != H2_PAL_OK) {
        (void)closesocket(accepted);
        return result;
    }
    if (out_peer_addr != NULL) {
        (void)windows_sockaddr_to_addr((SOCKADDR *)&address, out_peer_addr);
    }
    return H2_PAL_OK;
}

static int windows_net_tcp_send_timeout(void *user,
                                         h2_pal_net_socket_t token,
                                         const uint8_t *data, size_t len,
                                         uint32_t timeout_ms) {
    h2_windows_platform_t *platform = user;
    if ((data == NULL && len != 0u) || len > INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return 0;
    }
    h2_windows_socket_slot_t *slot = h2_windows_net_lock_slot(platform, token);
    if (slot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int result;
    if (slot->tls != NULL) {
        result = h2_windows_tls_send(platform, slot, data, len, timeout_ms);
    } else {
        result = windows_net_wait(slot->socket, 1, timeout_ms);
        if (result == H2_PAL_OK) {
            int sent = send(slot->socket, (const char *)data, (int)len, 0);
            result = sent == 0
                         ? H2_PAL_ERR_CLOSED
                         : sent == SOCKET_ERROR
                               ? h2_windows_error_from_wsa(WSAGetLastError())
                               : sent;
        }
    }
    h2_windows_net_unlock_slot(slot);
    return result;
}

static int windows_net_tcp_send(void *user, h2_pal_net_socket_t token,
                                const uint8_t *data, size_t len) {
    return windows_net_tcp_send_timeout(user, token, data, len, 0u);
}

static int windows_net_tcp_recv(void *user, h2_pal_net_socket_t token,
                                uint8_t *data, size_t len,
                                uint32_t timeout_ms) {
    h2_windows_platform_t *platform = user;
    if ((data == NULL && len != 0u) || len > INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return 0;
    }
    h2_windows_socket_slot_t *slot = h2_windows_net_lock_slot(platform, token);
    if (slot == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int result;
    if (slot->tls != NULL) {
        result = h2_windows_tls_recv(platform, slot, data, len, timeout_ms);
    } else {
        result = windows_net_wait(slot->socket, 0, timeout_ms);
        if (result == H2_PAL_OK) {
            int received = recv(slot->socket, (char *)data, (int)len, 0);
            result = received == 0
                         ? H2_PAL_ERR_CLOSED
                         : received == SOCKET_ERROR
                               ? h2_windows_error_from_wsa(WSAGetLastError())
                               : received;
        }
    }
    h2_windows_net_unlock_slot(slot);
    return result;
}

static int windows_net_get_host_addr(void *user, const char *iface_prefix,
                                     h2_pal_net_addr_t *out_addr) {
    return h2_windows_netif_host_addr(user, iface_prefix, out_addr);
}

static h2_pal_result_t windows_net_tls_wrap_entry(
    void *user, h2_pal_net_socket_t token,
    const h2_pal_net_tls_config_t *config, uint32_t timeout_ms,
    h2_pal_net_socket_t *out_token) {
    return h2_windows_tls_wrap(user, token, config, timeout_ms, out_token);
}

static h2_pal_result_t windows_net_icmp_echo(
    void *user, const h2_pal_net_addr_t *addr,
    const h2_pal_net_bind_t *binding, uint32_t timeout_ms,
    h2_pal_net_icmp_echo_result_t *out_result) {
    h2_windows_platform_t *platform = user;
    if (addr == NULL || out_result == NULL || timeout_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (binding != NULL && binding->type != H2_PAL_NET_BIND_DEFAULT &&
        binding->type != H2_PAL_NET_BIND_SOURCE_ADDR &&
        binding->type != H2_PAL_NET_BIND_NETIF) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(*out_result));
    static const uint8_t payload[] = "h2-pal-icmp";
    union {
        ULONGLONG alignment;
        uint8_t bytes[sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 64u];
    } reply;
    DWORD replies = 0u;
    if (addr->family == H2_PAL_NET_FAMILY_IPV4) {
        HANDLE handle = IcmpCreateFile();
        if (handle == INVALID_HANDLE_VALUE) {
            return h2_windows_error_from_win32(GetLastError());
        }
        IPAddr destination;
        memcpy(&destination, addr->ip, 4u);
        IPAddr source = INADDR_ANY;
        if (binding != NULL &&
            binding->type == H2_PAL_NET_BIND_SOURCE_ADDR) {
            if (binding->source_addr.family != H2_PAL_NET_FAMILY_IPV4) {
                (void)IcmpCloseHandle(handle);
                return H2_PAL_ERR_INVALID_ARG;
            }
            memcpy(&source, binding->source_addr.ip, 4u);
        } else if (binding != NULL &&
                   binding->type == H2_PAL_NET_BIND_NETIF) {
            h2_pal_netif_status_t status;
            h2_pal_result_t result = h2_windows_netif_vtable.get_status(
                platform, binding->netif, &status);
            if (result != H2_PAL_OK ||
                (status.flags & H2_PAL_NETIF_FLAG_HAS_IPV4) == 0u) {
                (void)IcmpCloseHandle(handle);
                return result != H2_PAL_OK ? result : H2_PAL_ERR_UNAVAILABLE;
            }
            memcpy(&source, status.ipv4.ip, 4u);
        }
        replies = IcmpSendEcho2Ex(
            handle, NULL, NULL, NULL, source, destination, (void *)payload,
            (WORD)sizeof(payload), NULL, reply.bytes,
            (DWORD)sizeof(reply.bytes),
            timeout_ms);
        (void)IcmpCloseHandle(handle);
    } else if (addr->family == H2_PAL_NET_FAMILY_IPV6) {
        HANDLE handle = Icmp6CreateFile();
        if (handle == INVALID_HANDLE_VALUE) {
            return h2_windows_error_from_win32(GetLastError());
        }
        SOCKADDR_IN6 source;
        SOCKADDR_IN6 destination;
        memset(&source, 0, sizeof(source));
        memset(&destination, 0, sizeof(destination));
        source.sin6_family = AF_INET6;
        destination.sin6_family = AF_INET6;
        memcpy(&destination.sin6_addr, addr->ip, 16u);
        if (binding != NULL &&
            binding->type == H2_PAL_NET_BIND_SOURCE_ADDR) {
            if (binding->source_addr.family != H2_PAL_NET_FAMILY_IPV6) {
                (void)IcmpCloseHandle(handle);
                return H2_PAL_ERR_INVALID_ARG;
            }
            memcpy(&source.sin6_addr, binding->source_addr.ip, 16u);
        } else if (binding != NULL &&
                   binding->type == H2_PAL_NET_BIND_NETIF) {
            h2_pal_netif_status_t status;
            h2_pal_result_t result = h2_windows_netif_vtable.get_status(
                platform, binding->netif, &status);
            if (result != H2_PAL_OK ||
                (status.flags & H2_PAL_NETIF_FLAG_HAS_IPV6) == 0u) {
                (void)IcmpCloseHandle(handle);
                return result != H2_PAL_OK ? result : H2_PAL_ERR_UNAVAILABLE;
            }
            memcpy(&source.sin6_addr, status.ipv6.ip, 16u);
            uint32_t scope_id = 0u;
            result = h2_windows_netif_resolve_ref_id(
                platform, binding->netif, H2_PAL_NET_FAMILY_IPV6,
                &scope_id);
            if (result != H2_PAL_OK) {
                (void)IcmpCloseHandle(handle);
                return result;
            }
            source.sin6_scope_id = scope_id;
        }
        replies = Icmp6SendEcho2(
            handle, NULL, NULL, NULL, &source, &destination, (void *)payload,
            (WORD)sizeof(payload), NULL, reply.bytes,
            (DWORD)sizeof(reply.bytes),
            timeout_ms);
        (void)IcmpCloseHandle(handle);
    } else {
        return H2_PAL_ERR_INVALID_ARG;
    }
    out_result->transmitted = 1u;
    if (replies == 0u) {
        DWORD error = GetLastError();
        return error == IP_REQ_TIMED_OUT ? H2_PAL_ERR_TIMEOUT
                                         : h2_windows_error_from_win32(error);
    }
    ULONG status = addr->family == H2_PAL_NET_FAMILY_IPV4
                       ? ((const ICMP_ECHO_REPLY *)reply.bytes)->Status
                       : ((const ICMPV6_ECHO_REPLY *)reply.bytes)->Status;
    if (status != IP_SUCCESS) {
        return status == IP_REQ_TIMED_OUT ? H2_PAL_ERR_TIMEOUT
                                          : H2_PAL_ERR_IO;
    }
    ULONG round_trip = addr->family == H2_PAL_NET_FAMILY_IPV4
                           ? ((const ICMP_ECHO_REPLY *)reply.bytes)
                                 ->RoundTripTime
                           : ((const ICMPV6_ECHO_REPLY *)reply.bytes)
                                 ->RoundTripTime;
    out_result->received = 1u;
    out_result->elapsed_ms = round_trip;
    return H2_PAL_OK;
}

static void windows_net_close(void *user, h2_pal_net_socket_t token) {
    h2_windows_platform_t *platform = user;
    EnterCriticalSection(&platform->lock);
    h2_windows_socket_slot_t *slot =
        windows_net_slot_locked(platform, token, NULL);
    if (slot == NULL) {
        LeaveCriticalSection(&platform->lock);
        return;
    }
    slot->in_use = 0;
    slot->closing = 1;
    LeaveCriticalSection(&platform->lock);
    EnterCriticalSection(&slot->lock);
    h2_windows_tls_release(slot);
    (void)closesocket(slot->socket);
    memset(&slot->connect_addr, 0, sizeof(slot->connect_addr));
    slot->socket = INVALID_SOCKET;
    slot->connecting = 0;
    LeaveCriticalSection(&slot->lock);
    EnterCriticalSection(&platform->lock);
    slot->closing = 0;
    LeaveCriticalSection(&platform->lock);
    h2_windows_object_release(platform);
}

h2_pal_result_t h2_windows_net_shutdown(h2_windows_platform_t *platform) {
    EnterCriticalSection(&platform->lock);
    for (size_t index = 0u; index < H2_WINDOWS_RESOLVER_CAPACITY; ++index) {
        h2_pal_net_resolver_t *resolver = platform->resolvers[index];
        if (resolver != NULL && resolver->cancel_handle != NULL) {
            (void)GetAddrInfoExCancel(&resolver->cancel_handle);
        }
    }
    while (platform->resolver_count != 0u) {
        (void)SleepConditionVariableCS(&platform->idle, &platform->lock,
                                       INFINITE);
    }
    for (size_t index = 0u; index < H2_WINDOWS_SOCKET_CAPACITY; ++index) {
        if (platform->sockets[index].in_use ||
            platform->sockets[index].closing) {
            LeaveCriticalSection(&platform->lock);
            return H2_PAL_ERR_BUSY;
        }
    }
    LeaveCriticalSection(&platform->lock);
    return H2_PAL_OK;
}

const h2_pal_net_vtable_t h2_windows_net_vtable = {
    .resolve_addr = windows_net_resolve_addr,
    .resolve_start = windows_net_resolve_start,
    .resolve_poll = windows_net_resolve_poll,
    .resolve_close = windows_net_resolve_close,
    .get_host_addr = windows_net_get_host_addr,
    .udp_open = windows_net_udp_open,
    .udp_sendto = windows_net_udp_sendto,
    .udp_recvfrom = windows_net_udp_recvfrom,
    .udp_open_bound = windows_net_udp_open_bound,
    .udp_join_multicast = windows_net_udp_join_multicast,
    .tcp_open = windows_net_tcp_open,
    .tcp_open_bound = windows_net_tcp_open_bound,
    .tcp_connect = windows_net_tcp_connect,
    .tcp_send = windows_net_tcp_send,
    .tcp_send_timeout = windows_net_tcp_send_timeout,
    .tcp_recv = windows_net_tcp_recv,
    .tls_wrap = windows_net_tls_wrap_entry,
    .icmp_echo = windows_net_icmp_echo,
    .close = windows_net_close,
    .tcp_listen = windows_net_tcp_listen,
    .tcp_accept = windows_net_tcp_accept,
};
