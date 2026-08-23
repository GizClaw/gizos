#include "h2_darwin_platform.h"
#include "h2_darwin_netif_internal.h"

#include <arpa/inet.h>
#include <assert.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <net/if_dl.h>
#elif defined(__linux__)
#include <netpacket/packet.h>
#endif

#define H2_DARWIN_NETIF_SNAPSHOT_MAX 64u

typedef struct h2_darwin_netif_snapshot {
    h2_pal_netif_status_t entries[H2_DARWIN_NETIF_SNAPSHOT_MAX];
    size_t count;
} h2_darwin_netif_snapshot_t;

typedef struct h2_darwin_netif_monitor_state {
    pthread_mutex_t mutex;
    pthread_t thread;
    h2_darwin_netif_os_monitor_t os;
    h2_pal_netif_ref_t current;
    int current_valid;
    int running;
    int thread_started;
} h2_darwin_netif_monitor_state_t;

static h2_darwin_netif_monitor_state_t s_monitor = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .os = {-1, -1, -1},
};

#if defined(H2_DARWIN_NETIF_TESTING)
static h2_darwin_netif_snapshot_t s_test_snapshot;
static h2_pal_netif_dns_server_t s_test_dns[H2_PAL_NETIF_DNS_MAX];
static size_t s_test_dns_count;
static int s_test_snapshot_enabled;
#endif

static h2_pal_netif_kind_t kind_from_name_flags(
    const char *name,
    unsigned int flags) {
    if ((flags & IFF_LOOPBACK) != 0 || strncmp(name, "lo", 2u) == 0) {
        return H2_PAL_NETIF_KIND_LOOPBACK;
    }
#if defined(__APPLE__)
    if (strncmp(name, "pdp_ip", 6u) == 0) {
        return H2_PAL_NETIF_KIND_MODEM_DATA;
    }
#endif
    return H2_PAL_NETIF_KIND_UNKNOWN;
}

static h2_pal_netif_status_t *snapshot_entry(
    h2_darwin_netif_snapshot_t *snapshot,
    const char *name,
    unsigned int flags) {
    for (size_t i = 0u; i < snapshot->count; ++i) {
        if (strncmp(snapshot->entries[i].ref.name, name,
                    H2_PAL_NETIF_NAME_MAX) == 0) {
            return &snapshot->entries[i];
        }
    }
    size_t name_len = strnlen(name, H2_PAL_NETIF_NAME_MAX);
    if (name_len == 0u || name_len >= H2_PAL_NETIF_NAME_MAX ||
        snapshot->count >= H2_DARWIN_NETIF_SNAPSHOT_MAX) {
        return NULL;
    }
    h2_pal_netif_status_t *status = &snapshot->entries[snapshot->count++];
    memset(status, 0, sizeof(*status));
    status->ref.type = H2_PAL_NETIF_REF_NAME;
    status->ref.kind = kind_from_name_flags(name, flags);
    memcpy(status->ref.name, name, name_len + 1u);
    status->kind = status->ref.kind;
    if ((flags & IFF_UP) != 0) {
        status->flags |= H2_PAL_NETIF_FLAG_UP;
    }
    if ((flags & IFF_RUNNING) != 0) {
        status->flags |= H2_PAL_NETIF_FLAG_LINK_UP;
    }
    if ((flags & IFF_POINTOPOINT) != 0) {
        status->flags |= H2_PAL_NETIF_FLAG_POINT_TO_POINT;
    }
    return status;
}

static void copy_sockaddr(
    h2_pal_net_addr_t *out,
    const struct sockaddr *addr) {
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        out->family = H2_PAL_NET_FAMILY_IPV4;
        memcpy(out->ip, &in->sin_addr, 4u);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
        out->family = H2_PAL_NET_FAMILY_IPV6;
        memcpy(out->ip, &in6->sin6_addr, 16u);
    }
}

static void fill_link_metadata(
    h2_pal_netif_status_t *status,
    const struct ifaddrs *item) {
#if defined(__APPLE__)
    if (item->ifa_addr->sa_family == AF_LINK) {
        const struct sockaddr_dl *link =
            (const struct sockaddr_dl *)item->ifa_addr;
        if (link->sdl_alen == 6u) {
            memcpy(status->mac, LLADDR(link), 6u);
            status->mac_valid = 1u;
        }
    }
#elif defined(__linux__)
    if (item->ifa_addr->sa_family == AF_PACKET) {
        const struct sockaddr_ll *link =
            (const struct sockaddr_ll *)item->ifa_addr;
        if (link->sll_halen == 6u) {
            memcpy(status->mac, link->sll_addr, 6u);
            status->mac_valid = 1u;
        }
    }
#endif
}

static void fill_mtu(h2_pal_netif_status_t *status) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    struct ifreq request;
    memset(&request, 0, sizeof(request));
    memcpy(request.ifr_name, status->ref.name,
           strnlen(status->ref.name, sizeof(request.ifr_name) - 1u));
    if (ioctl(fd, SIOCGIFMTU, &request) == 0 && request.ifr_mtu > 0 &&
        request.ifr_mtu <= UINT16_MAX) {
        status->mtu = (uint16_t)request.ifr_mtu;
    }
    close(fd);
}

static h2_pal_result_t take_snapshot(h2_darwin_netif_snapshot_t *snapshot) {
#if defined(H2_DARWIN_NETIF_TESTING)
    if (s_test_snapshot_enabled != 0) {
        *snapshot = s_test_snapshot;
        return H2_PAL_OK;
    }
#endif
    memset(snapshot, 0, sizeof(*snapshot));
    char default_name[H2_PAL_NETIF_NAME_MAX] = {0};
    h2_pal_result_t default_rc =
        h2_darwin_netif_os_default_name(default_name);
    if (default_rc != H2_PAL_OK && default_rc != H2_PAL_ERR_NOT_FOUND) {
        return default_rc;
    }
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) {
        return H2_PAL_ERR_IO;
    }
    for (const struct ifaddrs *item = interfaces; item != NULL;
         item = item->ifa_next) {
        if (item->ifa_name == NULL || item->ifa_addr == NULL) {
            continue;
        }
        h2_pal_netif_status_t *status = snapshot_entry(
            snapshot, item->ifa_name, item->ifa_flags);
        if (status == NULL) {
            continue;
        }
        if (item->ifa_addr->sa_family == AF_INET) {
            copy_sockaddr(&status->ipv4, item->ifa_addr);
            status->flags |= H2_PAL_NETIF_FLAG_HAS_IPV4;
            if (item->ifa_netmask != NULL) {
                copy_sockaddr(&status->netmask4, item->ifa_netmask);
            }
        } else if (item->ifa_addr->sa_family == AF_INET6 &&
                   (status->flags & H2_PAL_NETIF_FLAG_HAS_IPV6) == 0u) {
            copy_sockaddr(&status->ipv6, item->ifa_addr);
            status->flags |= H2_PAL_NETIF_FLAG_HAS_IPV6;
        }
        fill_link_metadata(status, item);
    }
    freeifaddrs(interfaces);
    for (size_t i = 0u; i < snapshot->count; ++i) {
        fill_mtu(&snapshot->entries[i]);
        if (default_rc == H2_PAL_OK &&
            strncmp(snapshot->entries[i].ref.name, default_name,
                    H2_PAL_NETIF_NAME_MAX) == 0) {
            snapshot->entries[i].flags |= H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
        }
    }
    return H2_PAL_OK;
}

static int filter_matches(
    const h2_pal_netif_filter_t *filter,
    const h2_pal_netif_status_t *status) {
    return filter == NULL ||
           ((filter->kind == H2_PAL_NETIF_KIND_UNKNOWN ||
             filter->kind == status->kind) &&
            (filter->name == NULL || filter->name[0] == '\0' ||
             strncmp(filter->name, status->ref.name,
                     H2_PAL_NETIF_NAME_MAX) == 0) &&
            (filter->id == 0u ||
             filter->id == if_nametoindex(status->ref.name)));
}

static h2_pal_result_t darwin_netif_list(
    void *user,
    const h2_pal_netif_filter_t *filter,
    h2_pal_netif_list_fn on_netif,
    void *callback_user) {
    (void)user;
    h2_darwin_netif_snapshot_t snapshot;
    h2_pal_result_t rc = take_snapshot(&snapshot);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (filter_matches(filter, &snapshot.entries[i]) &&
            on_netif(callback_user, &snapshot.entries[i].ref,
                     &snapshot.entries[i]) != 0) {
            return H2_PAL_EXIT;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t darwin_netif_find(
    void *user,
    const h2_pal_netif_filter_t *filter,
    h2_pal_netif_ref_t *out_ref) {
    (void)user;
    h2_darwin_netif_snapshot_t snapshot;
    h2_pal_result_t rc = take_snapshot(&snapshot);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (filter_matches(filter, &snapshot.entries[i])) {
            *out_ref = snapshot.entries[i].ref;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static int ref_matches(
    const h2_pal_netif_ref_t *ref,
    const h2_pal_netif_status_t *status) {
    if (ref->type == H2_PAL_NETIF_REF_NAME) {
        return memchr(ref->name, '\0', sizeof(ref->name)) != NULL &&
               strncmp(ref->name, status->ref.name,
                       H2_PAL_NETIF_NAME_MAX) == 0;
    }
    if (ref->type == H2_PAL_NETIF_REF_ID) {
        return ref->id != 0u && ref->id == if_nametoindex(status->ref.name);
    }
    if (ref->type == H2_PAL_NETIF_REF_KIND) {
        return ref->kind == status->kind;
    }
    return 0;
}

static h2_pal_result_t darwin_netif_get_status(
    void *user,
    const h2_pal_netif_ref_t *ref,
    h2_pal_netif_status_t *out_status) {
    (void)user;
    h2_darwin_netif_snapshot_t snapshot;
    h2_pal_result_t rc = take_snapshot(&snapshot);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if ((h2_pal_netif_ref_is_default(ref) &&
             (snapshot.entries[i].flags &
              H2_PAL_NETIF_FLAG_DEFAULT_ROUTE) != 0u) ||
            (ref != NULL && ref_matches(ref, &snapshot.entries[i]))) {
            *out_status = snapshot.entries[i];
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t darwin_netif_get_dns_servers(
    void *user,
    const h2_pal_netif_ref_t *ref,
    h2_pal_netif_dns_server_t *out_servers,
    size_t max_servers,
    size_t *out_count) {
    (void)user;
    *out_count = 0u;
    h2_pal_netif_status_t status;
    h2_pal_result_t rc = darwin_netif_get_status(NULL, ref, &status);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((status.flags & H2_PAL_NETIF_FLAG_DEFAULT_ROUTE) == 0u ||
        max_servers == 0u) {
        return H2_PAL_OK;
    }
#if defined(H2_DARWIN_NETIF_TESTING)
    if (s_test_snapshot_enabled != 0) {
        size_t count = s_test_dns_count < max_servers ?
            s_test_dns_count : max_servers;
        if (count > 0u) {
            memcpy(out_servers, s_test_dns,
                   count * sizeof(out_servers[0]));
        }
        *out_count = count;
        return H2_PAL_OK;
    }
#endif
    FILE *resolver = fopen("/etc/resolv.conf", "r");
    if (resolver == NULL) {
        return H2_PAL_ERR_IO;
    }
    char line[256];
    while (*out_count < max_servers && *out_count < H2_PAL_NETIF_DNS_MAX &&
           fgets(line, sizeof(line), resolver) != NULL) {
        char address[INET6_ADDRSTRLEN];
        if (sscanf(line, "nameserver %45s", address) != 1) {
            continue;
        }
        h2_pal_net_addr_t *out = &out_servers[*out_count].addr;
        memset(out, 0, sizeof(*out));
        if (inet_pton(AF_INET, address, out->ip) == 1) {
            out->family = H2_PAL_NET_FAMILY_IPV4;
        } else if (inet_pton(AF_INET6, address, out->ip) == 1) {
            out->family = H2_PAL_NET_FAMILY_IPV6;
        } else {
            continue;
        }
        ++*out_count;
    }
    fclose(resolver);
    return H2_PAL_OK;
}

static h2_pal_result_t default_ref(
    h2_pal_netif_ref_t *out_ref,
    int *out_valid) {
    char name[H2_PAL_NETIF_NAME_MAX] = {0};
    h2_pal_result_t rc = h2_darwin_netif_os_default_name(name);
    memset(out_ref, 0, sizeof(*out_ref));
    *out_valid = 0;
#if defined(H2_DARWIN_NETIF_TESTING)
    if (s_test_snapshot_enabled != 0) {
        for (size_t i = 0u; i < s_test_snapshot.count; ++i) {
            if ((s_test_snapshot.entries[i].flags &
                 H2_PAL_NETIF_FLAG_DEFAULT_ROUTE) != 0u) {
                *out_ref = s_test_snapshot.entries[i].ref;
                *out_valid = 1;
                return H2_PAL_OK;
            }
        }
        return H2_PAL_OK;
    }
#endif
    if (rc == H2_PAL_ERR_NOT_FOUND) {
        return H2_PAL_OK;
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    out_ref->type = H2_PAL_NETIF_REF_NAME;
    out_ref->kind = kind_from_name_flags(name, 0u);
    memcpy(out_ref->name, name, strnlen(name, sizeof(out_ref->name)) + 1u);
    *out_valid = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t monitor_reconcile_ref(
    const h2_pal_netif_ref_t *next,
    int next_valid) {
    h2_pal_netif_default_changed_t change;
    memset(&change, 0, sizeof(change));
    pthread_mutex_lock(&s_monitor.mutex);
    int changed = next_valid != s_monitor.current_valid ||
        (next_valid != 0 &&
         !h2_pal_netif_ref_equal(next, &s_monitor.current));
    if (changed != 0) {
        change.previous_valid = (uint8_t)s_monitor.current_valid;
        change.current_valid = (uint8_t)next_valid;
        if (s_monitor.current_valid != 0) {
            change.previous = s_monitor.current;
        }
        if (next_valid != 0) {
            change.current = *next;
        }
        s_monitor.current = *next;
        s_monitor.current_valid = next_valid;
    }
    pthread_mutex_unlock(&s_monitor.mutex);
    if (changed != 0) {
        h2_pal_system_event_t event = {
            .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
            .payload = &change,
            .payload_size = sizeof(change),
        };
        return h2_pal_system_event_post(
            h2_darwin_system_event_api(), &event, 0u);
    }
    return H2_PAL_OK;
}

static void monitor_reconcile(void) {
    h2_pal_netif_ref_t next;
    int next_valid = 0;
    h2_pal_result_t rc = default_ref(&next, &next_valid);
    if (rc == H2_PAL_OK) {
        rc = monitor_reconcile_ref(&next, next_valid);
    }
    if (rc != H2_PAL_OK) {
        (void)fprintf(stderr,
                      "H2_LOG level=warn scope=darwin_netif "
                      "message=default-route-reconcile-failed result=%d\n",
                      rc);
    }
}

static void *monitor_thread(void *user) {
    (void)user;
    for (;;) {
        h2_pal_result_t rc = h2_darwin_netif_os_monitor_wait(&s_monitor.os);
        pthread_mutex_lock(&s_monitor.mutex);
        int running = s_monitor.running;
        pthread_mutex_unlock(&s_monitor.mutex);
        if (running == 0) {
            break;
        }
        if (rc == H2_PAL_OK) {
            monitor_reconcile();
        }
    }
    return NULL;
}

h2_pal_result_t h2_darwin_netif_monitor_start(void) {
    pthread_mutex_lock(&s_monitor.mutex);
    if (s_monitor.running != 0) {
        pthread_mutex_unlock(&s_monitor.mutex);
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = default_ref(
        &s_monitor.current, &s_monitor.current_valid);
#if defined(H2_DARWIN_NETIF_TESTING)
    if (rc == H2_PAL_OK && s_test_snapshot_enabled != 0) {
        s_monitor.running = 1;
        pthread_mutex_unlock(&s_monitor.mutex);
        return H2_PAL_OK;
    }
#endif
    if (rc == H2_PAL_OK) {
        rc = h2_darwin_netif_os_monitor_open(&s_monitor.os);
    }
    if (rc == H2_PAL_OK) {
        s_monitor.running = 1;
        if (pthread_create(&s_monitor.thread, NULL, monitor_thread, NULL) != 0) {
            s_monitor.running = 0;
            rc = H2_PAL_ERR_IO;
        } else {
            s_monitor.thread_started = 1;
        }
    }
    pthread_mutex_unlock(&s_monitor.mutex);
    if (rc != H2_PAL_OK) {
        h2_darwin_netif_os_monitor_close(&s_monitor.os);
    }
    return rc;
}

void h2_darwin_netif_monitor_stop(void) {
    pthread_mutex_lock(&s_monitor.mutex);
    int join = s_monitor.thread_started;
    s_monitor.running = 0;
    pthread_mutex_unlock(&s_monitor.mutex);
    h2_darwin_netif_os_monitor_wake(&s_monitor.os);
    if (join != 0) {
        pthread_join(s_monitor.thread, NULL);
    }
    h2_darwin_netif_os_monitor_close(&s_monitor.os);
    pthread_mutex_lock(&s_monitor.mutex);
    s_monitor.thread_started = 0;
    s_monitor.current_valid = 0;
    memset(&s_monitor.current, 0, sizeof(s_monitor.current));
    pthread_mutex_unlock(&s_monitor.mutex);
}

#if defined(H2_DARWIN_NETIF_TESTING)
void h2_darwin_netif_test_set_snapshot(
    const h2_pal_netif_status_t *entries,
    size_t count,
    const h2_pal_netif_dns_server_t *dns,
    size_t dns_count) {
    assert(count <= H2_DARWIN_NETIF_SNAPSHOT_MAX);
    assert(dns_count <= H2_PAL_NETIF_DNS_MAX);
    memset(&s_test_snapshot, 0, sizeof(s_test_snapshot));
    if (count > 0u) {
        memcpy(s_test_snapshot.entries, entries,
               count * sizeof(entries[0]));
    }
    s_test_snapshot.count = count;
    memset(s_test_dns, 0, sizeof(s_test_dns));
    if (dns_count > 0u) {
        memcpy(s_test_dns, dns, dns_count * sizeof(dns[0]));
    }
    s_test_dns_count = dns_count;
    s_test_snapshot_enabled = 1;
}

void h2_darwin_netif_test_set_default(
    const h2_pal_netif_ref_t *ref,
    int valid) {
    pthread_mutex_lock(&s_monitor.mutex);
    memset(&s_monitor.current, 0, sizeof(s_monitor.current));
    if (valid != 0 && ref != NULL) {
        s_monitor.current = *ref;
        s_monitor.current_valid = 1;
    } else {
        s_monitor.current_valid = 0;
    }
    pthread_mutex_unlock(&s_monitor.mutex);
}

h2_pal_result_t h2_darwin_netif_test_reconcile_default(
    const h2_pal_netif_ref_t *ref,
    int valid) {
    h2_pal_netif_ref_t zero;
    memset(&zero, 0, sizeof(zero));
    return monitor_reconcile_ref(
        valid != 0 && ref != NULL ? ref : &zero,
        valid != 0 && ref != NULL);
}
#endif

const h2_pal_netif_api_t *h2_darwin_netif_api(void) {
    static const h2_pal_netif_vtable_t vtable = {
        .list = darwin_netif_list,
        .find = darwin_netif_find,
        .get_status = darwin_netif_get_status,
        .get_dns_servers = darwin_netif_get_dns_servers,
    };
    static const h2_pal_netif_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
