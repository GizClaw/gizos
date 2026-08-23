#include "h2_darwin_platform.h"
#include "h2_darwin_netif_internal.h"

#include <assert.h>
#include <net/if.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

typedef struct observed_changes {
    h2_pal_netif_default_changed_t changes[4];
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
    args->result = h2_darwin_netif_test_reconcile_default(args->ref, 1);
    return NULL;
}

static void *deinit_thread(void *user) {
    deinit_thread_args_t *args = user;
    atomic_store_explicit(&args->started, 1, memory_order_release);
    h2_pal_system_event_deinit(args->events);
    atomic_store_explicit(&args->done, 1, memory_order_release);
    return NULL;
}

static int observe_change(void *user, const h2_pal_system_event_t *event) {
    observed_changes_t *observed = user;
    assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED);
    assert(event->payload_size == sizeof(h2_pal_netif_default_changed_t));
    assert(observed->count < 4u);
    observed->changes[observed->count++] =
        *(const h2_pal_netif_default_changed_t *)event->payload;
    h2_pal_result_t result = observed->next_result;
    observed->next_result = H2_PAL_OK;
    return result;
}

static h2_pal_netif_ref_t named_ref(
    const char *name,
    h2_pal_netif_kind_t kind) {
    h2_pal_netif_ref_t ref = {
        .type = H2_PAL_NETIF_REF_NAME,
        .kind = kind,
    };
    size_t len = strlen(name);
    assert(len < sizeof(ref.name));
    memcpy(ref.name, name, len + 1u);
    return ref;
}

typedef struct listed_netifs {
    h2_pal_netif_status_t entries[4];
    size_t count;
} listed_netifs_t;

static int collect_netif(
    void *user,
    const h2_pal_netif_ref_t *ref,
    const h2_pal_netif_status_t *status) {
    listed_netifs_t *listed = user;
    assert(listed->count < 4u);
    assert(h2_pal_netif_ref_is_concrete(ref));
    assert(h2_pal_netif_ref_equal(ref, &status->ref));
    listed->entries[listed->count++] = *status;
    return 0;
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

int main(void) {
    const char *default_name = existing_interface_name();
    h2_pal_netif_status_t fixture[2];
    memset(fixture, 0, sizeof(fixture));
    fixture[0].ref = named_ref(default_name, H2_PAL_NETIF_KIND_WIFI_STA);
    fixture[0].kind = fixture[0].ref.kind;
    fixture[0].flags = H2_PAL_NETIF_FLAG_UP |
        H2_PAL_NETIF_FLAG_LINK_UP | H2_PAL_NETIF_FLAG_HAS_IPV4 |
        H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
    fixture[0].ipv4.family = H2_PAL_NET_FAMILY_IPV4;
    fixture[0].ipv4.ip[0] = 192u;
    fixture[0].ipv4.ip[1] = 0u;
    fixture[0].ipv4.ip[2] = 2u;
    fixture[0].ipv4.ip[3] = 10u;
    fixture[0].mtu = 1500u;
    memset(fixture[0].mac, 0x11, sizeof(fixture[0].mac));
    fixture[0].mac_valid = 1u;
    fixture[1].ref = named_ref("fixture-ppp", H2_PAL_NETIF_KIND_MODEM_DATA);
    fixture[1].kind = fixture[1].ref.kind;
    fixture[1].flags = H2_PAL_NETIF_FLAG_UP |
        H2_PAL_NETIF_FLAG_LINK_UP | H2_PAL_NETIF_FLAG_POINT_TO_POINT;
    fixture[1].mtu = 1420u;
    h2_pal_netif_dns_server_t dns = {0};
    dns.addr.family = H2_PAL_NET_FAMILY_IPV4;
    dns.addr.ip[0] = 1u;
    dns.addr.ip[1] = 1u;
    dns.addr.ip[2] = 1u;
    dns.addr.ip[3] = 1u;
    h2_darwin_netif_test_set_snapshot(fixture, 2u, &dns, 1u);

    const h2_pal_netif_api_t *netif = h2_darwin_netif_api();
    listed_netifs_t listed = {0};
    assert(h2_pal_netif_list(netif, NULL, collect_netif, &listed) ==
           H2_PAL_OK);
    assert(listed.count == 2u);
    assert(listed.entries[0].mtu == 1500u);
    assert(listed.entries[0].mac_valid == 1u);
    assert((listed.entries[0].flags & H2_PAL_NETIF_FLAG_HAS_IPV4) != 0u);

    h2_pal_netif_filter_t filter = {
        .kind = H2_PAL_NETIF_KIND_MODEM_DATA,
        .name = "fixture-ppp",
    };
    h2_pal_netif_ref_t found;
    assert(h2_pal_netif_find(netif, &filter, &found) == H2_PAL_OK);
    assert(h2_pal_netif_ref_equal(&found, &fixture[1].ref));

    h2_pal_netif_status_t status;
    assert(h2_pal_netif_get_status(netif, NULL, &status) == H2_PAL_OK);
    assert(h2_pal_netif_ref_equal(&status.ref, &fixture[0].ref));
    h2_pal_netif_ref_t id_ref = {
        .type = H2_PAL_NETIF_REF_ID,
        .id = if_nametoindex(default_name),
    };
    assert(id_ref.id != 0u);
    assert(h2_pal_netif_get_status(netif, &id_ref, &status) == H2_PAL_OK);
    h2_pal_netif_ref_t stale = named_ref("stale0", H2_PAL_NETIF_KIND_UNKNOWN);
    assert(h2_pal_netif_get_status(netif, &stale, &status) ==
           H2_PAL_ERR_NOT_FOUND);

    h2_pal_netif_dns_server_t servers[H2_PAL_NETIF_DNS_MAX];
    size_t server_count = 0u;
    assert(h2_pal_netif_get_dns_servers(
               netif, NULL, servers, H2_PAL_NETIF_DNS_MAX,
               &server_count) == H2_PAL_OK);
    assert(server_count == 1u && servers[0].addr.ip[0] == 1u);
    assert(h2_pal_netif_get_dns_servers(
               netif, &fixture[1].ref, servers,
               H2_PAL_NETIF_DNS_MAX, &server_count) == H2_PAL_OK);
    assert(server_count == 0u);

    const h2_pal_system_event_api_t *events =
        h2_darwin_system_event_api();
    assert(h2_pal_system_event_init(events) == H2_PAL_OK);
    observed_changes_t observed = {0};
    h2_pal_system_event_subscription_t *subscription = NULL;
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
               observe_change, &observed, &subscription) == H2_PAL_OK);

    observed.next_result = H2_PAL_ERR_IO;
    assert(h2_darwin_netif_test_reconcile_default(
               &fixture[1].ref, 1) == H2_PAL_ERR_IO);
    assert(observed.count == 1u);
    assert(h2_pal_netif_ref_equal(
        &observed.changes[0].previous, &fixture[0].ref));
    assert(h2_pal_netif_ref_equal(
        &observed.changes[0].current, &fixture[1].ref));
    assert(h2_darwin_netif_test_reconcile_default(
               &fixture[1].ref, 1) == H2_PAL_OK);
    assert(observed.count == 1u);

    fixture[0].flags &= ~H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
    h2_darwin_netif_test_set_snapshot(fixture, 2u, &dns, 1u);
    assert(h2_pal_netif_get_status(netif, NULL, &status) ==
           H2_PAL_ERR_NOT_FOUND);
    assert(h2_darwin_netif_test_reconcile_default(NULL, 0) == H2_PAL_OK);
    assert(observed.count == 2u);
    assert(observed.changes[1].previous_valid == 1u);
    assert(observed.changes[1].current_valid == 0u);

    assert(h2_darwin_netif_test_reconcile_default(
               &fixture[0].ref, 1) == H2_PAL_OK);
    assert(observed.count == 3u);
    assert(observed.changes[2].previous_valid == 0u);
    assert(observed.changes[2].current_valid == 1u);

    h2_pal_system_event_unsubscribe(events, subscription);

    blocking_observer_t blocking = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
    };
    assert(h2_pal_system_event_subscribe(
               events, H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
               observe_change_blocking, &blocking, &subscription) ==
           H2_PAL_OK);
    reconcile_thread_args_t reconcile = {
        .ref = &fixture[1].ref,
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
    return 0;
}
