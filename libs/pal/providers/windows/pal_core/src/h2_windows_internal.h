#ifndef H2_WINDOWS_INTERNAL_H
#define H2_WINDOWS_INTERNAL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include "h2_windows_platform.h"

#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#include <stddef.h>
#include <stdint.h>

#define H2_WINDOWS_SOCKET_CAPACITY 64u
#define H2_WINDOWS_RESOLVER_CAPACITY 16u
#define H2_WINDOWS_SUBSCRIPTION_CAPACITY 48u

typedef struct h2_windows_mount {
    wchar_t *source;
    wchar_t *target;
    size_t source_len;
    size_t target_len;
} h2_windows_mount_t;

typedef struct h2_windows_socket_slot {
    CRITICAL_SECTION lock;
    SOCKET socket;
    WOLFSSL *tls;
    WOLFSSL_CTX *tls_context;
    uint32_t generation;
    h2_pal_net_addr_t connect_addr;
    int in_use;
    int closing;
    int connecting;
} h2_windows_socket_slot_t;

typedef struct h2_windows_route_candidate {
    uint32_t interface_id;
    uint32_t route_metric;
    uint32_t interface_metric;
    int connected;
} h2_windows_route_candidate_t;

struct h2_windows_platform {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE idle;
    LARGE_INTEGER qpc_frequency;
    LONG live_allocations;
    LONG live_objects;
    LONG shutting_down;
    h2_windows_mount_t *mounts;
    size_t mount_count;
    h2_windows_socket_slot_t sockets[H2_WINDOWS_SOCKET_CAPACITY];
    size_t resolver_count;
    h2_pal_net_resolver_t *resolvers[H2_WINDOWS_RESOLVER_CAPACITY];
    int system_event_initialized;
    HANDLE route_notification;
    PTP_WORK route_work;
    LONG route_dirty;
    LONG route_stopping;
    h2_pal_netif_ref_t default_netif;
    int default_netif_valid;
    h2_pal_system_event_subscription_t
        *subscriptions[H2_WINDOWS_SUBSCRIPTION_CAPACITY];

    h2_pal_mem_api_t mem_api;
    h2_pal_log_api_t log_api;
    h2_pal_time_api_t time_api;
    h2_pal_timer_api_t timer_api;
    h2_pal_task_api_t task_api;
    h2_pal_queue_api_t queue_api;
    h2_pal_sync_api_t sync_api;
    h2_pal_fs_api_t fs_api;
    h2_pal_net_api_t net_api;
    h2_pal_netif_api_t netif_api;
    h2_pal_system_event_api_t system_event_api;
};

void *h2_windows_heap_alloc(size_t size);
void *h2_windows_heap_realloc(void *memory, size_t size);
void h2_windows_heap_free(void *memory);
void h2_windows_object_acquire(h2_windows_platform_t *platform);
void h2_windows_object_release(h2_windows_platform_t *platform);
h2_pal_result_t h2_windows_error_from_win32(DWORD error);
h2_pal_result_t h2_windows_error_from_wsa(int error);
h2_pal_result_t h2_windows_monotonic_ms(
    h2_windows_platform_t *platform,
    uint64_t *out_ms);
wchar_t *h2_windows_utf8_to_wide(const char *text);
char *h2_windows_wide_to_utf8(const wchar_t *text);

extern const h2_pal_mem_vtable_t h2_windows_mem_vtable;
extern const h2_pal_log_vtable_t h2_windows_log_vtable;
extern const h2_pal_time_vtable_t h2_windows_time_vtable;
extern const h2_pal_timer_vtable_t h2_windows_timer_vtable;
extern const h2_pal_task_vtable_t h2_windows_task_vtable;
extern const h2_pal_queue_vtable_t h2_windows_queue_vtable;
extern const h2_pal_sync_vtable_t h2_windows_sync_vtable;
extern const h2_pal_fs_vtable_t h2_windows_fs_vtable;
extern const h2_pal_net_vtable_t h2_windows_net_vtable;
extern const h2_pal_netif_vtable_t h2_windows_netif_vtable;
extern const h2_pal_system_event_vtable_t h2_windows_system_event_vtable;

h2_pal_result_t h2_windows_net_shutdown(h2_windows_platform_t *platform);
h2_pal_result_t h2_windows_system_event_shutdown(
    h2_windows_platform_t *platform);
h2_pal_result_t h2_windows_netif_default_ref(
    h2_windows_platform_t *platform,
    h2_pal_netif_ref_t *out_ref,
    int *out_valid);
h2_pal_result_t h2_windows_netif_resolve_ref_id(
    h2_windows_platform_t *platform,
    const h2_pal_netif_ref_t *ref,
    h2_pal_net_family_t family,
    uint32_t *out_id);
h2_pal_result_t h2_windows_netif_host_addr(
    h2_windows_platform_t *platform,
    const char *iface_prefix,
    h2_pal_net_addr_t *out_addr);
uint32_t h2_windows_netif_select_default(
    const h2_windows_route_candidate_t *candidates,
    size_t count);

h2_pal_result_t h2_windows_tls_wrap(
    h2_windows_platform_t *platform,
    h2_pal_net_socket_t token,
    const h2_pal_net_tls_config_t *config,
    uint32_t timeout_ms,
    h2_pal_net_socket_t *out_token);
int h2_windows_tls_send(
    h2_windows_platform_t *platform,
    h2_windows_socket_slot_t *slot,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms);
int h2_windows_tls_recv(
    h2_windows_platform_t *platform,
    h2_windows_socket_slot_t *slot,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms);
void h2_windows_tls_release(h2_windows_socket_slot_t *slot);

h2_windows_socket_slot_t *h2_windows_net_lock_slot(
    h2_windows_platform_t *platform,
    h2_pal_net_socket_t token);
void h2_windows_net_unlock_slot(h2_windows_socket_slot_t *slot);

#endif
