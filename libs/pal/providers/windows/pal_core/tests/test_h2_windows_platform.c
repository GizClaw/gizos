#include "h2_windows_platform.h"
#include "h2_wolfssl.h"

#include <assert.h>
#include <string.h>
#include <windows.h>

static void test_task_entry(void *context) {
    volatile LONG *value = context;
    (void)InterlockedIncrement(value);
}

static void test_timer_callback(void *user, h2_pal_timer_t *timer) {
    (void)timer;
    (void)SetEvent((HANDLE)user);
}

static int test_netif_callback(void *user, const h2_pal_netif_ref_t *ref,
                               const h2_pal_netif_status_t *status) {
    int *count = user;
    assert(ref != NULL);
    assert(status != NULL);
    ++*count;
    return 1;
}

static int test_event_callback(void *user,
                               const h2_pal_system_event_t *event) {
    int *count = user;
    assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED);
    ++*count;
    return 0;
}

static void make_temp_directory(wchar_t *out_path, size_t capacity,
                                char *out_utf8, size_t utf8_capacity) {
    wchar_t temp[MAX_PATH];
    wchar_t candidate[MAX_PATH];
    assert(GetTempPathW((DWORD)(sizeof(temp) / sizeof(temp[0])), temp) != 0u);
    assert(GetTempFileNameW(temp, L"h2p", 0u, candidate) != 0u);
    assert(DeleteFileW(candidate));
    assert(CreateDirectoryW(candidate, NULL));
    assert(wcsncpy_s(out_path, capacity, candidate, _TRUNCATE) == 0);
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, candidate, -1,
                               out_utf8, (int)utf8_capacity, NULL, NULL) > 0);
}

int main(void) {
    wchar_t temp_path[MAX_PATH];
    char temp_utf8[MAX_PATH * 4u];
    make_temp_directory(temp_path, sizeof(temp_path) / sizeof(temp_path[0]),
                        temp_utf8, sizeof(temp_utf8));
    const char *sources[] = {temp_utf8};
    const char *targets[] = {"/data"};
    h2_windows_platform_config_t platform_config = {
        .fs_sources = sources,
        .fs_targets = targets,
        .fs_mount_count = 1u,
    };
    h2_windows_platform_t *platform = NULL;
    assert(h2_windows_platform_create(&platform_config, &platform) ==
           H2_PAL_OK);
    assert(platform != NULL);

    const h2_pal_mem_api_t *mem = h2_windows_mem_api(platform);
    void *allocation = h2_pal_mem_alloc(mem, 17u);
    assert(allocation != NULL);
    allocation = h2_pal_mem_realloc(mem, allocation, 33u);
    assert(allocation != NULL);
    h2_pal_mem_free(mem, allocation);

    char long_message[H2_PAL_LOG_MESSAGE_MAX + 2u];
    memset(long_message, 'x', sizeof(long_message) - 1u);
    long_message[sizeof(long_message) - 1u] = '\0';
    assert(h2_pal_log_write(h2_windows_log_api(platform), H2_PAL_LOG_INFO,
                            "windows-test", long_message) ==
           H2_PAL_ERR_TRUNCATED);
    uint8_t entropy[32];
    assert(h2_windows_entropy(platform, entropy, sizeof(entropy)) ==
           H2_PAL_OK);

    uint64_t monotonic = 0u;
    uint64_t wall = 0u;
    assert(h2_pal_time_get_monotonic_ms(h2_windows_time_api(platform),
                                        &monotonic) == H2_PAL_OK);
    assert(h2_pal_time_get_wall_ms(h2_windows_time_api(platform), &wall) ==
           H2_PAL_OK);
    assert(wall != 0u);
    h2_pal_time_wall_status_t wall_status;
    assert(h2_pal_time_get_wall_status(h2_windows_time_api(platform),
                                       &wall_status) == H2_PAL_OK);
    assert(wall_status.valid != 0u);
    const h2_pal_time_api_t *time = h2_windows_time_api(platform);
    assert(time->vtable->get_wall_ms(time->user, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(time->vtable->get_wall_status(time->user, NULL) ==
           H2_PAL_ERR_INVALID_ARG);

    volatile LONG task_value = 0;
    h2_pal_task_t *task = NULL;
    assert(h2_pal_task_start(h2_windows_task_api(platform), NULL,
                             test_task_entry, (void *)&task_value, &task) ==
           H2_PAL_OK);
    assert(h2_pal_task_join(h2_windows_task_api(platform), task) == H2_PAL_OK);
    assert(task_value == 1);

    HANDLE timer_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(timer_event != NULL);
    h2_pal_timer_config_t timer_config = {
        .name = "test",
        .period_ms = 5u,
        .flags = H2_PAL_TIMER_FLAG_AUTO_START,
        .cb = test_timer_callback,
        .cb_user = timer_event,
    };
    h2_pal_timer_t *timer = NULL;
    assert(h2_pal_timer_create(h2_windows_timer_api(platform), &timer_config,
                               &timer) == H2_PAL_OK);
    assert(WaitForSingleObject(timer_event, 2000u) == WAIT_OBJECT_0);
    assert(h2_pal_timer_destroy(h2_windows_timer_api(platform), timer) ==
           H2_PAL_OK);
    assert(CloseHandle(timer_event));

    h2_pal_queue_config_t queue_config = {
        .name = "test",
        .item_size = sizeof(uint32_t),
        .item_count = 2u,
        .allocator = mem,
    };
    h2_pal_queue_t *queue = NULL;
    assert(h2_pal_queue_create(h2_windows_queue_api(platform), &queue_config,
                               &queue) == H2_PAL_OK);
    uint32_t sent = 42u;
    uint32_t received = 0u;
    assert(h2_pal_queue_send(h2_windows_queue_api(platform), queue, &sent,
                             H2_PAL_QUEUE_NO_WAIT) == H2_PAL_OK);
    assert(h2_pal_queue_recv(h2_windows_queue_api(platform), queue, &received,
                             H2_PAL_QUEUE_NO_WAIT) == H2_PAL_OK);
    assert(received == sent);
    assert(h2_pal_queue_close(h2_windows_queue_api(platform), queue) ==
           H2_PAL_OK);
    h2_pal_queue_destroy(h2_windows_queue_api(platform), queue);

    h2_pal_mutex_config_t mutex_config = {
        .name = "test",
        .allocator = mem,
        .flags = H2_PAL_MUTEX_FLAG_NONE,
    };
    h2_pal_mutex_t *mutex = NULL;
    assert(h2_pal_mutex_create(h2_windows_sync_api(platform), &mutex_config,
                               &mutex) == H2_PAL_OK);
    assert(h2_pal_mutex_lock(h2_windows_sync_api(platform), mutex) ==
           H2_PAL_OK);
    assert(h2_pal_mutex_unlock(h2_windows_sync_api(platform), mutex) ==
           H2_PAL_OK);
    assert(h2_pal_mutex_destroy(h2_windows_sync_api(platform), mutex) ==
           H2_PAL_OK);
    const h2_pal_fs_api_t *fs = h2_windows_fs_api(platform);
    assert(h2_pal_fs_mkdir(fs, "/data/nested") == H2_PAL_OK);
    h2_pal_fs_file_t *file = NULL;
    assert(h2_pal_fs_open(fs, "/data/nested/value", H2_PAL_FS_OPEN_WRITE_TRUNCATE,
                          &file) == H2_PAL_OK);
    const char payload[] = "windows-pal";
    size_t transferred = 0u;
    assert(h2_pal_fs_write(fs, file, payload, sizeof(payload), &transferred) ==
           H2_PAL_OK);
    assert(transferred == sizeof(payload));
    assert(h2_pal_fs_sync(fs, file) == H2_PAL_OK);
    assert(h2_pal_fs_close(fs, file) == H2_PAL_OK);
    assert(h2_pal_fs_open(fs, "/data/nested/value", H2_PAL_FS_OPEN_READ,
                          &file) == H2_PAL_OK);
    char buffer[sizeof(payload)] = {0};
    assert(h2_pal_fs_read(fs, file, buffer, sizeof(buffer), &transferred) ==
           H2_PAL_OK);
    assert(transferred == sizeof(buffer));
    assert(memcmp(buffer, payload, sizeof(payload)) == 0);
    assert(h2_pal_fs_close(fs, file) == H2_PAL_OK);
    assert(h2_pal_fs_remove(fs, "/data/nested/value") == H2_PAL_OK);
    assert(h2_pal_fs_remove(fs, "/data/nested") == H2_PAL_OK);
    h2_pal_fs_stat_t stat_value;
    assert(h2_pal_fs_stat(fs, "/data/../escape", &stat_value) ==
           H2_PAL_ERR_INVALID_ARG);

    const h2_pal_net_api_t *net = h2_windows_net_api(platform);
    h2_pal_net_addr_t resolved;
    assert(h2_pal_net_resolve_addr(net, "localhost", &resolved) == H2_PAL_OK);
    h2_pal_net_resolver_t *resolver = NULL;
    assert(h2_pal_net_resolve_start(net, "localhost", &resolver) == H2_PAL_OK);
    assert(h2_pal_net_resolve_poll(net, resolver, &resolved, 5000u) ==
           H2_PAL_OK);
    h2_pal_net_resolve_close(net, resolver);

    h2_pal_net_socket_t udp = -1;
    h2_pal_net_addr_t bound;
    assert(h2_pal_net_udp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, 0u, NULL,
                                      &udp, &bound) == H2_PAL_OK);
    h2_pal_net_addr_t loopback = {
        .family = H2_PAL_NET_FAMILY_IPV4,
        .port = bound.port,
        .ip = {127u, 0u, 0u, 1u},
    };
    assert(h2_pal_net_udp_sendto(net, udp, &loopback,
                                 (const uint8_t *)payload,
                                 sizeof(payload)) == (int)sizeof(payload));
    h2_pal_net_addr_t peer;
    assert(h2_pal_net_udp_recvfrom(net, udp, &peer, (uint8_t *)buffer,
                                   sizeof(buffer), 2000u) ==
           (int)sizeof(buffer));
    h2_pal_net_close(net, udp);

    h2_pal_net_socket_t tcp = -1;
    assert(h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, NULL, &tcp) ==
           H2_PAL_OK);
    h2_wolfssl_config_t wolfssl_config = {
        .mem = *mem,
        .entropy_user = platform,
        .entropy = h2_windows_entropy,
    };
    assert(h2_wolfssl_init(&wolfssl_config) == H2_PAL_OK);
    h2_pal_net_tls_config_t tls_config = {
        .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
    };
    h2_pal_net_socket_t tls = -1;
    assert(h2_pal_net_tls_wrap(net, tcp, &tls_config, 10u, &tls) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(tls == -1);
    h2_pal_net_close(net, tcp);
    assert(h2_wolfssl_deinit() == H2_PAL_OK);

    int netif_count = 0;
    assert(h2_pal_netif_list(h2_windows_netif_api(platform), NULL,
                             test_netif_callback, &netif_count) == H2_PAL_EXIT);
    assert(netif_count == 1);

    const h2_pal_system_event_api_t *events =
        h2_windows_system_event_api(platform);
    assert(h2_pal_system_event_init(events) == H2_PAL_OK);
    h2_pal_system_event_subscription_t *subscription = NULL;
    int event_count = 0;
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
               test_event_callback, &event_count, &subscription) == H2_PAL_OK);
    h2_pal_system_event_t event = {
        .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
    };
    assert(h2_pal_system_event_post(events, &event, 0u) == H2_PAL_OK);
    assert(event_count == 1);
    h2_pal_system_event_unsubscribe(events, subscription);
    h2_pal_system_event_deinit(events);

    assert(h2_windows_platform_destroy(&platform) == H2_PAL_OK);
    assert(platform == NULL);
    assert(RemoveDirectoryW(temp_path));
    return 0;
}
