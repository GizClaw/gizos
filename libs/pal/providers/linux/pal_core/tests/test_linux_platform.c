#include "h2_linux_platform.h"

#include <assert.h>
#include <net/if.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Private test hook, compiled only with H2_LINUX_TESTING. */
uint32_t h2_linux_display_test_rgb565_to_argb8888(uint16_t pixel);
int h2_linux_display_test_back_page_yoffset(
    uint32_t visible_height,
    uint32_t virtual_height,
    uint32_t current_yoffset,
    uint32_t line_length,
    size_t memory_size,
    uint32_t *out_yoffset);
int h2_linux_display_test_copy_page(
    uint8_t *memory,
    size_t memory_size,
    uint32_t line_length,
    uint32_t page_height,
    uint32_t source_yoffset,
    uint32_t destination_yoffset);
int h2_linux_display_test_use_framebuffer(
    uint8_t *memory,
    size_t memory_size,
    uint32_t width,
    uint32_t height,
    uint32_t line_length);

typedef struct task_fixture {
    atomic_int ran;
    int value;
} task_fixture_t;

static void task_entry(void *user) {
    task_fixture_t *fixture = user;
    fixture->value = 42;
    atomic_store_explicit(&fixture->ran, 1, memory_order_release);
}

#if defined(__linux__)
typedef struct observed_changes {
    h2_pal_netif_default_changed_t changes[3];
    size_t count;
    h2_pal_result_t next_result;
} observed_changes_t;

typedef struct blocking_observer {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int entered;
    int release;
} blocking_observer_t;

typedef struct reconcile_thread_args {
    const h2_pal_netif_ref_t *ref;
    h2_pal_result_t result;
} reconcile_thread_args_t;

typedef struct deinit_thread_args {
    const h2_pal_system_event_api_t *events;
    atomic_int started;
    atomic_int done;
} deinit_thread_args_t;

static int count_netif(
    void *user,
    const h2_pal_netif_ref_t *ref,
    const h2_pal_netif_status_t *status) {
    size_t *count = user;
    assert(h2_pal_netif_ref_is_concrete(ref));
    assert(h2_pal_netif_ref_equal(ref, &status->ref));
    ++*count;
    return 0;
}

static int observe_change(void *user, const h2_pal_system_event_t *event) {
    observed_changes_t *observed = user;
    assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED);
    assert(event->payload_size == sizeof(h2_pal_netif_default_changed_t));
    assert(observed->count < 3u);
    observed->changes[observed->count++] =
        *(const h2_pal_netif_default_changed_t *)event->payload;
    h2_pal_result_t result = observed->next_result;
    observed->next_result = H2_PAL_OK;
    return result;
}

static int observe_change_blocking(
    void *user,
    const h2_pal_system_event_t *event) {
    blocking_observer_t *observer = user;
    assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED);
    pthread_mutex_lock(&observer->mutex);
    observer->entered = 1;
    pthread_cond_broadcast(&observer->condition);
    while (observer->release == 0) {
        pthread_cond_wait(&observer->condition, &observer->mutex);
    }
    pthread_mutex_unlock(&observer->mutex);
    return H2_PAL_OK;
}

static void *reconcile_thread(void *user) {
    reconcile_thread_args_t *args = user;
    args->result = h2_linux_netif_test_reconcile_default(
        args->ref, 1);
    return NULL;
}

static void *deinit_thread(void *user) {
    deinit_thread_args_t *args = user;
    atomic_store_explicit(&args->started, 1, memory_order_release);
    h2_pal_system_event_deinit(args->events);
    atomic_store_explicit(&args->done, 1, memory_order_release);
    return NULL;
}

static h2_pal_netif_ref_t named_ref(const char *name) {
    h2_pal_netif_ref_t ref = {
        .type = H2_PAL_NETIF_REF_NAME,
        .kind = H2_PAL_NETIF_KIND_UNKNOWN,
    };
    size_t len = strlen(name);
    assert(len < sizeof(ref.name));
    memcpy(ref.name, name, len + 1u);
    return ref;
}

static const char *existing_interface_name(void) {
    static char name[H2_PAL_NETIF_NAME_MAX];
    struct if_nameindex *interfaces = if_nameindex();
    assert(interfaces != NULL && interfaces[0].if_index != 0u);
    size_t length = strnlen(interfaces[0].if_name, sizeof(name));
    assert(length > 0u && length < sizeof(name));
    memcpy(name, interfaces[0].if_name, length + 1u);
    if_freenameindex(interfaces);
    return name;
}
#endif

int main(void) {
    assert(h2_linux_display_test_rgb565_to_argb8888(0xffffu) == UINT32_C(0xffffffff));
    assert(h2_linux_display_test_rgb565_to_argb8888(0xf800u) == UINT32_C(0xffff0000));
    assert(h2_linux_display_test_rgb565_to_argb8888(0x07e0u) == UINT32_C(0xff00ff00));
    assert(h2_linux_display_test_rgb565_to_argb8888(0x001fu) == UINT32_C(0xff0000ff));
    assert(h2_linux_display_test_rgb565_to_argb8888(0x0000u) == UINT32_C(0xff000000));
    uint32_t back_yoffset = UINT32_MAX;
    assert(h2_linux_display_test_back_page_yoffset(
               600u, 1200u, 0u, 4096u, 4096u * 1200u,
               &back_yoffset) == 1);
    assert(back_yoffset == 600u);
    assert(h2_linux_display_test_back_page_yoffset(
               600u, 1200u, 600u, 4096u, 4096u * 1200u,
               &back_yoffset) == 1);
    assert(back_yoffset == 0u);
    assert(h2_linux_display_test_back_page_yoffset(
               600u, 600u, 0u, 4096u, 4096u * 600u,
               &back_yoffset) == 0);
    assert(h2_linux_display_test_back_page_yoffset(
               600u, 1200u, 100u, 4096u, 4096u * 1200u,
               &back_yoffset) == 0);

    static const uint8_t initial_page[] = {
        0x10u, 0x11u, 0x12u, 0x13u, 0x20u, 0x21u, 0x22u, 0x23u,
    };
    uint8_t framebuffer[sizeof(initial_page) * 2u];
    memcpy(framebuffer, initial_page, sizeof(initial_page));
    memset(framebuffer + sizeof(initial_page), 0xcc, sizeof(initial_page));
    assert(h2_linux_display_test_copy_page(
               framebuffer, sizeof(framebuffer), 4u, 2u, 0u, 2u) == 1);
    assert(memcmp(
               framebuffer,
               framebuffer + sizeof(initial_page),
               sizeof(initial_page)) == 0);

    framebuffer[sizeof(initial_page) + 2u] = 0xa5u;
    uint8_t expected_page[sizeof(initial_page)];
    memcpy(
        expected_page,
        framebuffer + sizeof(initial_page),
        sizeof(expected_page));
    assert(h2_linux_display_test_copy_page(
               framebuffer, sizeof(framebuffer), 4u, 2u, 2u, 0u) == 1);
    assert(memcmp(framebuffer, expected_page, sizeof(expected_page)) == 0);
    assert(memcmp(
               framebuffer + sizeof(initial_page),
               expected_page,
               sizeof(expected_page)) == 0);
    assert(h2_linux_display_test_copy_page(
               framebuffer, sizeof(framebuffer) - 1u, 4u, 2u, 2u, 0u) == 0);

    uint16_t lifecycle_framebuffer[] = {
        0x1001u, 0x1002u, 0x2001u, 0x2002u,
        0xccccu, 0xccccu, 0xccccu, 0xccccu,
    };
    const uint16_t partial_pixel = 0xa5a5u;
    const uint16_t expected_lifecycle_page[] = {
        0x1001u, 0xa5a5u, 0x2001u, 0x2002u,
    };
    const h2_display_rect_t partial_rect = {
        .x = 1,
        .y = 0,
        .width = 1,
        .height = 1,
    };
    assert(h2_linux_display_test_use_framebuffer(
               (uint8_t *)lifecycle_framebuffer,
               sizeof(lifecycle_framebuffer),
               2u,
               2u,
               2u * sizeof(uint16_t)) == 1);
    const h2_pal_display_api_t *display = h2_linux_display_api();
    assert(display->vtable->draw_bitmap(
               display->user,
               &partial_rect,
               &partial_pixel,
               sizeof(partial_pixel),
               H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
    assert(display->vtable->present(display->user) == H2_PAL_OK);
    assert(memcmp(
               lifecycle_framebuffer + 4u,
               expected_lifecycle_page,
               sizeof(expected_lifecycle_page)) == 0);
    assert(display->vtable->present(display->user) == H2_PAL_OK);
    assert(memcmp(
               lifecycle_framebuffer,
               expected_lifecycle_page,
               sizeof(expected_lifecycle_page)) == 0);
    assert(memcmp(
               lifecycle_framebuffer + 4u,
               expected_lifecycle_page,
               sizeof(expected_lifecycle_page)) == 0);

    uint64_t first = 0u;
    uint64_t second = 0u;
    assert(h2_pal_time_get_monotonic_ms(h2_linux_time_api(), &first) == H2_PAL_OK);
    assert(h2_pal_time_sleep_ms(h2_linux_time_api(), 1u) == H2_PAL_OK);
    assert(h2_pal_time_get_monotonic_ms(h2_linux_time_api(), &second) == H2_PAL_OK);
    assert(second >= first);

    task_fixture_t task_fixture = {0};
    atomic_init(&task_fixture.ran, 0);
    const h2_pal_task_options_t task_options = {
        .name = "linux-task-test",
        .min_stack_size = 32768u,
    };
    h2_pal_task_t *task = NULL;
    assert(h2_linux_task_api()->vtable->start(
               h2_linux_task_api()->user,
               &task_options,
               NULL,
               &task_fixture,
               &task) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_linux_task_api()->vtable->start(
               h2_linux_task_api()->user,
               &task_options,
               task_entry,
               &task_fixture,
               NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_linux_task_api()->vtable->join(
               h2_linux_task_api()->user,
               NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_task_start(
               h2_linux_task_api(),
               &task_options,
               task_entry,
               &task_fixture,
               &task) == H2_PAL_OK);
    assert(task != NULL);
    assert(h2_pal_task_join(h2_linux_task_api(), task) == H2_PAL_OK);
    assert(atomic_load_explicit(&task_fixture.ran, memory_order_acquire) == 1);
    assert(task_fixture.value == 42);

    const h2_pal_queue_config_t config = {
        .name = "test",
        .item_size = sizeof(uint32_t),
        .item_count = 2u,
        .allocator = h2_linux_mem_api(),
    };
    h2_pal_queue_t *queue = NULL;
    assert(h2_pal_queue_create(h2_linux_queue_api(), &config, &queue) == H2_PAL_OK);
    uint32_t value = 1u;
    assert(h2_pal_queue_send(h2_linux_queue_api(), queue, &value, 0u) == H2_PAL_OK);
    value = 2u;
    assert(h2_pal_queue_send(h2_linux_queue_api(), queue, &value, 0u) == H2_PAL_OK);
    value = 3u;
    assert(h2_pal_queue_send_latest(h2_linux_queue_api(), queue, &value) == H2_PAL_OK);
    uint32_t output = 0u;
    assert(h2_pal_queue_recv(h2_linux_queue_api(), queue, &output, 0u) == H2_PAL_OK);
    assert(output == 2u);
    assert(h2_pal_queue_recv(h2_linux_queue_api(), queue, &output, 0u) == H2_PAL_OK);
    assert(output == 3u);
    uint64_t wait_started = 0u;
    uint64_t wait_finished = 0u;
    assert(h2_pal_time_get_monotonic_ms(h2_linux_time_api(), &wait_started) ==
           H2_PAL_OK);
    assert(h2_pal_queue_recv(h2_linux_queue_api(), queue, &output, 5u) ==
           H2_PAL_ERR_TIMEOUT);
    assert(h2_pal_time_get_monotonic_ms(h2_linux_time_api(), &wait_finished) ==
           H2_PAL_OK);
    assert(wait_finished >= wait_started + 5u);
    assert(wait_finished < wait_started + 1000u);
    assert(h2_pal_queue_close(h2_linux_queue_api(), queue) == H2_PAL_OK);
    assert(h2_pal_queue_recv(h2_linux_queue_api(), queue, &output, 0u) == H2_PAL_ERR_CLOSED);
    h2_pal_queue_destroy(h2_linux_queue_api(), queue);

#if defined(__linux__)
    const char *default_name = existing_interface_name();
    h2_pal_netif_status_t fixture[2];
    memset(fixture, 0, sizeof(fixture));
    fixture[0].ref = named_ref(default_name);
    fixture[0].ref.kind = H2_PAL_NETIF_KIND_ETHERNET;
    fixture[0].kind = fixture[0].ref.kind;
    fixture[0].flags = H2_PAL_NETIF_FLAG_UP |
        H2_PAL_NETIF_FLAG_LINK_UP | H2_PAL_NETIF_FLAG_HAS_IPV4 |
        H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
    fixture[0].ipv4.family = H2_PAL_NET_FAMILY_IPV4;
    fixture[0].ipv4.ip[0] = 192u;
    fixture[0].ipv4.ip[1] = 0u;
    fixture[0].ipv4.ip[2] = 2u;
    fixture[0].ipv4.ip[3] = 20u;
    fixture[0].mtu = 1500u;
    memset(fixture[0].mac, 0x22, sizeof(fixture[0].mac));
    fixture[0].mac_valid = 1u;
    fixture[1].ref = named_ref("wlan-fixture");
    fixture[1].ref.kind = H2_PAL_NETIF_KIND_WIFI_STA;
    fixture[1].kind = fixture[1].ref.kind;
    fixture[1].flags = H2_PAL_NETIF_FLAG_UP |
        H2_PAL_NETIF_FLAG_LINK_UP;
    h2_pal_netif_dns_server_t dns = {0};
    dns.addr.family = H2_PAL_NET_FAMILY_IPV4;
    dns.addr.ip[0] = 9u;
    dns.addr.ip[1] = 9u;
    dns.addr.ip[2] = 9u;
    dns.addr.ip[3] = 9u;
    h2_linux_netif_test_set_snapshot(
        fixture, 2u, &dns, 1u);

    size_t netif_count = 0u;
    assert(h2_pal_netif_list(h2_linux_netif_api(), NULL,
                            count_netif, &netif_count) == H2_PAL_OK);
    assert(netif_count == 2u);
    h2_pal_netif_filter_t filter = {
        .kind = H2_PAL_NETIF_KIND_WIFI_STA,
        .name = "wlan-fixture",
    };
    h2_pal_netif_ref_t found;
    assert(h2_pal_netif_find(
               h2_linux_netif_api(), &filter, &found) ==
           H2_PAL_OK);
    assert(h2_pal_netif_ref_equal(&found, &fixture[1].ref));
    h2_pal_netif_status_t status;
    assert(h2_pal_netif_get_status(
               h2_linux_netif_api(), NULL, &status) ==
           H2_PAL_OK);
    assert(status.mtu == 1500u && status.mac_valid == 1u);
    h2_pal_netif_ref_t id_ref = {
        .type = H2_PAL_NETIF_REF_ID,
        .id = if_nametoindex(default_name),
    };
    assert(id_ref.id != 0u);
    assert(h2_pal_netif_get_status(
               h2_linux_netif_api(), &id_ref, &status) ==
           H2_PAL_OK);
    h2_pal_netif_ref_t stale = named_ref("stale0");
    assert(h2_pal_netif_get_status(
               h2_linux_netif_api(), &stale, &status) ==
           H2_PAL_ERR_NOT_FOUND);
    h2_pal_netif_dns_server_t servers[H2_PAL_NETIF_DNS_MAX];
    size_t server_count = 0u;
    assert(h2_pal_netif_get_dns_servers(
               h2_linux_netif_api(), NULL, servers,
               H2_PAL_NETIF_DNS_MAX, &server_count) == H2_PAL_OK);
    assert(server_count == 1u && servers[0].addr.ip[0] == 9u);
    assert(h2_pal_netif_get_dns_servers(
               h2_linux_netif_api(), &fixture[1].ref, servers,
               H2_PAL_NETIF_DNS_MAX, &server_count) == H2_PAL_OK);
    assert(server_count == 0u);
    const h2_pal_system_event_api_t *events =
        h2_linux_system_event_api();
    assert(h2_pal_system_event_init(events) == H2_PAL_OK);
    observed_changes_t observed = {0};
    h2_pal_system_event_subscription_t *subscription = NULL;
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
               observe_change, &observed, &subscription) == H2_PAL_OK);
    const h2_pal_netif_ref_t wifi = fixture[0].ref;
    const h2_pal_netif_ref_t modem = named_ref("modem-test");
    assert(h2_linux_netif_test_reconcile_default(&wifi, 1) ==
           H2_PAL_OK);
    assert(observed.count == 0u);
    observed.next_result = H2_PAL_ERR_IO;
    assert(h2_linux_netif_test_reconcile_default(&modem, 1) ==
           H2_PAL_ERR_IO);
    assert(observed.count == 1u);
    assert(h2_pal_netif_ref_equal(&observed.changes[0].previous, &wifi));
    assert(h2_pal_netif_ref_equal(&observed.changes[0].current, &modem));
    assert(h2_linux_netif_test_reconcile_default(NULL, 0) ==
           H2_PAL_OK);
    assert(observed.count == 2u);
    assert(observed.changes[1].current_valid == 0u);
    fixture[0].flags &= ~H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
    h2_linux_netif_test_set_snapshot(
        fixture, 2u, &dns, 1u);
    assert(h2_pal_netif_get_status(
               h2_linux_netif_api(), NULL, &status) ==
           H2_PAL_ERR_NOT_FOUND);
    h2_pal_system_event_unsubscribe(events, subscription);

    h2_linux_netif_test_set_default(&wifi, 1);
    blocking_observer_t blocking = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
    };
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
               observe_change_blocking, &blocking, &subscription) ==
           H2_PAL_OK);
    reconcile_thread_args_t reconcile = {
        .ref = &modem,
        .result = H2_PAL_ERR_INVALID_STATE,
    };
    pthread_t posting_thread;
    assert(pthread_create(&posting_thread, NULL, reconcile_thread,
                          &reconcile) == 0);
    pthread_mutex_lock(&blocking.mutex);
    while (blocking.entered == 0) {
        pthread_cond_wait(&blocking.condition, &blocking.mutex);
    }
    pthread_mutex_unlock(&blocking.mutex);
    deinit_thread_args_t deinit = {.events = events};
    atomic_init(&deinit.started, 0);
    atomic_init(&deinit.done, 0);
    pthread_t stopping_thread;
    assert(pthread_create(&stopping_thread, NULL, deinit_thread, &deinit) == 0);
    while (atomic_load_explicit(&deinit.started, memory_order_acquire) == 0) {
    }
    const struct timespec drain_check = {.tv_nsec = 10000000L};
    assert(nanosleep(&drain_check, NULL) == 0);
    assert(atomic_load_explicit(&deinit.done, memory_order_acquire) == 0);
    pthread_mutex_lock(&blocking.mutex);
    blocking.release = 1;
    pthread_cond_broadcast(&blocking.condition);
    pthread_mutex_unlock(&blocking.mutex);
    assert(pthread_join(posting_thread, NULL) == 0);
    assert(pthread_join(stopping_thread, NULL) == 0);
    assert(atomic_load_explicit(&deinit.done, memory_order_acquire) == 1);
    assert(reconcile.result == H2_PAL_OK);
    pthread_cond_destroy(&blocking.condition);
    pthread_mutex_destroy(&blocking.mutex);
#else
    h2_pal_netif_ref_t default_ref = h2_pal_netif_default_ref();
    h2_pal_netif_status_t status;
    assert(h2_pal_netif_get_status(h2_linux_netif_api(),
                                   &default_ref, &status) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_system_event_init(
               h2_linux_system_event_api()) ==
           H2_PAL_ERR_UNSUPPORTED);
#endif
    return 0;
}
