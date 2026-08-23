#define _POSIX_C_SOURCE 200809L

#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "h2_linux_platform.h"

#include <errno.h>
#include <arpa/inet.h>
#include <assert.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netpacket/packet.h>
#endif

#if !defined(H2_LINUX_NO_FB)
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#define H2_LINUX_QUEUE_CLOCK CLOCK_MONOTONIC
#else
#define H2_LINUX_QUEUE_CLOCK CLOCK_REALTIME
#endif

static void *linux_alloc(void *user, size_t size) {
    (void)user;
    return malloc(size);
}

static void *linux_realloc(void *user, void *memory, size_t size) {
    (void)user;
    return realloc(memory, size);
}

static void linux_free(void *user, void *memory) {
    (void)user;
    free(memory);
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = linux_alloc,
    .realloc = linux_realloc,
    .free = linux_free,
};
static const h2_pal_mem_api_t s_mem_api = {.user = NULL, .vtable = &s_mem_vtable};

const h2_pal_mem_api_t *h2_linux_mem_api(void) {
    return &s_mem_api;
}

static int linux_log_write(
    void *user,
    h2_pal_log_level_t level,
    const char *scope,
    const char *message) {
    (void)user;
    static const char *const levels[] = {"debug", "info", "warn", "error"};
    const char *level_name = level >= H2_PAL_LOG_DEBUG && level <= H2_PAL_LOG_ERROR
                                 ? levels[level]
                                 : "unknown";
    if (fprintf(stderr, "H2_LOG level=%s scope=%s message=%s\n", level_name,
                scope != NULL ? scope : "h2", message) < 0) {
        return H2_PAL_ERR_WRITE;
    }
    return H2_PAL_OK;
}

static const h2_pal_log_vtable_t s_log_vtable = {.write = linux_log_write};
static const h2_pal_log_api_t s_log_api = {.user = NULL, .vtable = &s_log_vtable};

const h2_pal_log_api_t *h2_linux_log_api(void) {
    return &s_log_api;
}

static h2_pal_result_t clock_ms(clockid_t clock_id, uint64_t *out_ms) {
    struct timespec value;
    if (clock_gettime(clock_id, &value) != 0) return H2_PAL_ERR_IO;
    *out_ms = (uint64_t)value.tv_sec * UINT64_C(1000) + (uint64_t)value.tv_nsec / UINT64_C(1000000);
    return H2_PAL_OK;
}

static h2_pal_result_t clock_us(clockid_t clock_id, uint64_t *out_us) {
    struct timespec value;
    if (clock_gettime(clock_id, &value) != 0) return H2_PAL_ERR_IO;
    *out_us = (uint64_t)value.tv_sec * UINT64_C(1000000) +
              (uint64_t)value.tv_nsec / UINT64_C(1000);
    return H2_PAL_OK;
}

static h2_pal_result_t linux_monotonic_ms(void *user, uint64_t *out_ms) {
    (void)user;
    return clock_ms(CLOCK_MONOTONIC, out_ms);
}

static h2_pal_result_t linux_monotonic_us(void *user, uint64_t *out_us) {
    (void)user;
    return clock_us(CLOCK_MONOTONIC, out_us);
}

static h2_pal_result_t linux_wall_ms(void *user, uint64_t *out_ms) {
    (void)user;
    return clock_ms(CLOCK_REALTIME, out_ms);
}

static h2_pal_result_t linux_set_wall_ms(void *user, uint64_t wall_ms) {
    (void)user;
    (void)wall_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t linux_wall_status(void *user, h2_pal_time_wall_status_t *out_status) {
    (void)user;
    *out_status = (h2_pal_time_wall_status_t){
        .valid = 1u,
        .source = H2_PAL_TIME_WALL_SOURCE_RTC,
    };
    return H2_PAL_OK;
}

static h2_pal_result_t linux_sleep_ms(void *user, uint32_t milliseconds) {
    (void)user;
    struct timespec remaining = {
        .tv_sec = (time_t)(milliseconds / 1000u),
        .tv_nsec = (long)(milliseconds % 1000u) * 1000000L,
    };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) return H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static const h2_pal_time_vtable_t s_time_vtable = {
    .get_monotonic_ms = linux_monotonic_ms,
    .get_monotonic_us = linux_monotonic_us,
    .get_wall_ms = linux_wall_ms,
    .set_wall_ms = linux_set_wall_ms,
    .get_wall_status = linux_wall_status,
    .sleep_ms = linux_sleep_ms,
};
static const h2_pal_time_api_t s_time_api = {.user = NULL, .vtable = &s_time_vtable};

const h2_pal_time_api_t *h2_linux_time_api(void) {
    return &s_time_api;
}

struct h2_pal_task {
    pthread_t thread;
    h2_pal_task_entry_t entry;
    void *ctx;
};

static void *linux_task_entry(void *user) {
    h2_pal_task_t *task = user;
    task->entry(task->ctx);
    return NULL;
}

static int linux_task_start(
    void *user,
    const h2_pal_task_options_t *options,
    h2_pal_task_entry_t entry,
    void *ctx,
    h2_pal_task_t **out_task) {
    (void)user;
    if (entry == NULL || out_task == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_task = NULL;
    h2_pal_task_t *task = malloc(sizeof(*task));
    if (task == NULL) return H2_PAL_ERR_NO_MEMORY;
    task->entry = entry;
    task->ctx = ctx;

    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        free(task);
        return H2_PAL_ERR_TASK;
    }
    if (options != NULL && options->min_stack_size > 0u) {
        size_t stack_size = options->min_stack_size;
        if (stack_size < PTHREAD_STACK_MIN) stack_size = PTHREAD_STACK_MIN;
        if (pthread_attr_setstacksize(&attributes, stack_size) != 0) {
            (void)pthread_attr_destroy(&attributes);
            free(task);
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    const int create_result = pthread_create(
        &task->thread,
        &attributes,
        linux_task_entry,
        task);
    (void)pthread_attr_destroy(&attributes);
    if (create_result != 0) {
        free(task);
        return create_result == EAGAIN ? H2_PAL_ERR_NO_MEMORY : H2_PAL_ERR_TASK;
    }
    *out_task = task;
    return H2_PAL_OK;
}

static int linux_task_join(void *user, h2_pal_task_t *task) {
    (void)user;
    if (task == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (pthread_join(task->thread, NULL) != 0) return H2_PAL_ERR_TASK;
    free(task);
    return H2_PAL_OK;
}

static const h2_pal_task_vtable_t s_task_vtable = {
    .start = linux_task_start,
    .join = linux_task_join,
};
static const h2_pal_task_api_t s_task_api = {
    .user = NULL,
    .vtable = &s_task_vtable,
};

const h2_pal_task_api_t *h2_linux_task_api(void) {
    return &s_task_api;
}

struct h2_pal_queue {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    const h2_pal_mem_api_t *allocator;
    uint8_t *items;
    size_t item_size;
    size_t item_count;
    size_t head;
    size_t count;
    int closed;
};

static int queue_wait(
    pthread_cond_t *condition,
    pthread_mutex_t *mutex,
    uint32_t timeout_ms,
    const struct timespec *deadline) {
    if (timeout_ms == H2_PAL_QUEUE_NO_WAIT) return H2_PAL_ERR_TIMEOUT;
    if (timeout_ms == H2_PAL_QUEUE_WAIT_FOREVER) {
        return pthread_cond_wait(condition, mutex) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
    }
    const int rc = pthread_cond_timedwait(condition, mutex, deadline);
    if (rc == ETIMEDOUT) return H2_PAL_ERR_TIMEOUT;
    return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int queue_deadline(uint32_t timeout_ms, struct timespec *deadline) {
    if (timeout_ms == H2_PAL_QUEUE_NO_WAIT || timeout_ms == H2_PAL_QUEUE_WAIT_FOREVER) {
        return H2_PAL_OK;
    }
    if (clock_gettime(H2_LINUX_QUEUE_CLOCK, deadline) != 0) return H2_PAL_ERR_IO;
    deadline->tv_sec += (time_t)(timeout_ms / 1000u);
    deadline->tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        ++deadline->tv_sec;
        deadline->tv_nsec -= 1000000000L;
    }
    return H2_PAL_OK;
}

static int queue_create(
    void *user,
    const h2_pal_queue_config_t *config,
    h2_pal_queue_t **out_queue) {
    (void)user;
    *out_queue = NULL;
    if (config->item_size == 0u || config->item_count == 0u || config->allocator == NULL ||
        config->item_count > SIZE_MAX / config->item_size) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_queue_t *queue = h2_pal_mem_alloc(config->allocator, sizeof(*queue));
    if (queue == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(queue, 0, sizeof(*queue));
    queue->allocator = config->allocator;
    queue->item_size = config->item_size;
    queue->item_count = config->item_count;
    queue->items = h2_pal_mem_alloc(config->allocator, config->item_size * config->item_count);
    if (queue->items == NULL) {
        h2_pal_mem_free(config->allocator, queue->items);
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        h2_pal_mem_free(config->allocator, queue->items);
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_IO;
    }
    pthread_condattr_t condition_attributes;
    if (pthread_condattr_init(&condition_attributes) != 0) {
        (void)pthread_mutex_destroy(&queue->mutex);
        h2_pal_mem_free(config->allocator, queue->items);
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_IO;
    }
#if defined(__linux__)
    if (pthread_condattr_setclock(&condition_attributes, H2_LINUX_QUEUE_CLOCK) != 0) {
        (void)pthread_condattr_destroy(&condition_attributes);
        (void)pthread_mutex_destroy(&queue->mutex);
        h2_pal_mem_free(config->allocator, queue->items);
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_IO;
    }
#endif
    if (pthread_cond_init(&queue->not_empty, &condition_attributes) != 0) {
        (void)pthread_condattr_destroy(&condition_attributes);
        (void)pthread_mutex_destroy(&queue->mutex);
        h2_pal_mem_free(config->allocator, queue->items);
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_IO;
    }
    if (pthread_cond_init(&queue->not_full, &condition_attributes) != 0) {
        (void)pthread_condattr_destroy(&condition_attributes);
        (void)pthread_cond_destroy(&queue->not_empty);
        (void)pthread_mutex_destroy(&queue->mutex);
        h2_pal_mem_free(config->allocator, queue->items);
        h2_pal_mem_free(config->allocator, queue);
        return H2_PAL_ERR_IO;
    }
    (void)pthread_condattr_destroy(&condition_attributes);
    *out_queue = queue;
    return H2_PAL_OK;
}

static void queue_destroy(void *user, h2_pal_queue_t *queue) {
    (void)user;
    const h2_pal_mem_api_t *allocator = queue->allocator;
    (void)pthread_cond_destroy(&queue->not_empty);
    (void)pthread_cond_destroy(&queue->not_full);
    (void)pthread_mutex_destroy(&queue->mutex);
    h2_pal_mem_free(allocator, queue->items);
    h2_pal_mem_free(allocator, queue);
}

static int queue_send(
    void *user,
    h2_pal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms) {
    (void)user;
    (void)pthread_mutex_lock(&queue->mutex);
    struct timespec deadline = {0};
    int rc = queue_deadline(timeout_ms, &deadline);
    if (rc != H2_PAL_OK) {
        (void)pthread_mutex_unlock(&queue->mutex);
        return rc;
    }
    while (!queue->closed && queue->count == queue->item_count) {
        rc = queue_wait(&queue->not_full, &queue->mutex, timeout_ms, &deadline);
        if (rc != H2_PAL_OK) {
            (void)pthread_mutex_unlock(&queue->mutex);
            return rc;
        }
    }
    if (queue->closed) {
        (void)pthread_mutex_unlock(&queue->mutex);
        return H2_PAL_ERR_CLOSED;
    }
    const size_t tail = (queue->head + queue->count) % queue->item_count;
    memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    (void)pthread_cond_signal(&queue->not_empty);
    (void)pthread_mutex_unlock(&queue->mutex);
    return H2_PAL_OK;
}

static int queue_send_latest(void *user, h2_pal_queue_t *queue, const void *item) {
    (void)user;
    (void)pthread_mutex_lock(&queue->mutex);
    if (queue->closed) {
        (void)pthread_mutex_unlock(&queue->mutex);
        return H2_PAL_ERR_CLOSED;
    }
    if (queue->count == queue->item_count) {
        queue->head = (queue->head + 1u) % queue->item_count;
        --queue->count;
    }
    const size_t tail = (queue->head + queue->count) % queue->item_count;
    memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    (void)pthread_cond_signal(&queue->not_empty);
    (void)pthread_mutex_unlock(&queue->mutex);
    return H2_PAL_OK;
}

static int queue_recv(
    void *user,
    h2_pal_queue_t *queue,
    void *out_item,
    uint32_t timeout_ms) {
    (void)user;
    (void)pthread_mutex_lock(&queue->mutex);
    struct timespec deadline = {0};
    int rc = queue_deadline(timeout_ms, &deadline);
    if (rc != H2_PAL_OK) {
        (void)pthread_mutex_unlock(&queue->mutex);
        return rc;
    }
    while (!queue->closed && queue->count == 0u) {
        rc = queue_wait(&queue->not_empty, &queue->mutex, timeout_ms, &deadline);
        if (rc != H2_PAL_OK) {
            (void)pthread_mutex_unlock(&queue->mutex);
            return rc;
        }
    }
    if (queue->count == 0u && queue->closed) {
        (void)pthread_mutex_unlock(&queue->mutex);
        return H2_PAL_ERR_CLOSED;
    }
    memcpy(out_item, queue->items + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1u) % queue->item_count;
    --queue->count;
    (void)pthread_cond_signal(&queue->not_full);
    (void)pthread_mutex_unlock(&queue->mutex);
    return H2_PAL_OK;
}

static int queue_reset(void *user, h2_pal_queue_t *queue) {
    (void)user;
    (void)pthread_mutex_lock(&queue->mutex);
    queue->head = 0u;
    queue->count = 0u;
    (void)pthread_cond_broadcast(&queue->not_full);
    (void)pthread_mutex_unlock(&queue->mutex);
    return H2_PAL_OK;
}

static int queue_close(void *user, h2_pal_queue_t *queue) {
    (void)user;
    (void)pthread_mutex_lock(&queue->mutex);
    queue->closed = 1;
    (void)pthread_cond_broadcast(&queue->not_empty);
    (void)pthread_cond_broadcast(&queue->not_full);
    (void)pthread_mutex_unlock(&queue->mutex);
    return H2_PAL_OK;
}

static const h2_pal_queue_vtable_t s_queue_vtable = {
    .create = queue_create,
    .destroy = queue_destroy,
    .send = queue_send,
    .send_latest = queue_send_latest,
    .recv = queue_recv,
    .reset = queue_reset,
    .close = queue_close,
};
static const h2_pal_queue_api_t s_queue_api = {.user = NULL, .vtable = &s_queue_vtable};

const h2_pal_queue_api_t *h2_linux_queue_api(void) {
    return &s_queue_api;
}

#define H2_LINUX_NETIF_SNAPSHOT_MAX 64u
#define H2_LINUX_SYSTEM_EVENT_MAX_SUBSCRIPTIONS 48u

typedef struct linux_netif_snapshot {
    h2_pal_netif_status_t entries[H2_LINUX_NETIF_SNAPSHOT_MAX];
    size_t count;
} linux_netif_snapshot_t;

#if defined(__linux__) && defined(H2_LINUX_TESTING)
static linux_netif_snapshot_t s_linux_test_snapshot;
static h2_pal_netif_dns_server_t
    s_linux_test_dns[H2_PAL_NETIF_DNS_MAX];
static size_t s_linux_test_dns_count;
static int s_linux_test_snapshot_enabled;
#endif

#if defined(__linux__)
static h2_pal_netif_kind_t linux_netif_kind(
    const char *name,
    unsigned int flags) {
    if ((flags & IFF_LOOPBACK) != 0 || strncmp(name, "lo", 2u) == 0) {
        return H2_PAL_NETIF_KIND_LOOPBACK;
    }
    return H2_PAL_NETIF_KIND_UNKNOWN;
}

static h2_pal_result_t linux_route_socket(
    uint32_t groups,
    int *out_fd) {
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    struct sockaddr_nl address;
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    address.nl_groups = groups;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd);
        return H2_PAL_ERR_IO;
    }
    *out_fd = fd;
    return H2_PAL_OK;
}

static h2_pal_result_t linux_default_name(
    char out_name[H2_PAL_NETIF_NAME_MAX]) {
    int fd = -1;
    h2_pal_result_t rc = linux_route_socket(0u, &fd);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    struct {
        struct nlmsghdr header;
        struct rtmsg route;
    } request;
    memset(&request, 0, sizeof(request));
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.route));
    request.header.nlmsg_type = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = 1u;
    request.route.rtm_family = AF_INET;
    if (send(fd, &request, request.header.nlmsg_len, 0) < 0) {
        (void)close(fd);
        return H2_PAL_ERR_IO;
    }

    uint32_t best_priority = UINT32_MAX;
    unsigned int best_index = 0u;
    uint8_t buffer[8192];
    for (;;) {
        ssize_t size = recv(fd, buffer, sizeof(buffer), 0);
        if (size <= 0) {
            break;
        }
        for (struct nlmsghdr *header = (struct nlmsghdr *)buffer;
             NLMSG_OK(header, size); header = NLMSG_NEXT(header, size)) {
            if (header->nlmsg_type == NLMSG_DONE) {
                (void)close(fd);
                if (best_index == 0u ||
                    if_indextoname(best_index, out_name) == NULL) {
                    return H2_PAL_ERR_NOT_FOUND;
                }
                return H2_PAL_OK;
            }
            if (header->nlmsg_type == NLMSG_ERROR) {
                (void)close(fd);
                return H2_PAL_ERR_IO;
            }
            if (header->nlmsg_type != RTM_NEWROUTE) {
                continue;
            }
            const struct rtmsg *route =
                (const struct rtmsg *)NLMSG_DATA(header);
            if (route->rtm_family != AF_INET || route->rtm_dst_len != 0u ||
                route->rtm_table != RT_TABLE_MAIN ||
                route->rtm_type != RTN_UNICAST) {
                continue;
            }
            unsigned int index = 0u;
            uint32_t priority = 0u;
            int attr_size = RTM_PAYLOAD(header);
            for (struct rtattr *attr = RTM_RTA(route);
                 RTA_OK(attr, attr_size);
                 attr = RTA_NEXT(attr, attr_size)) {
                if (attr->rta_type == RTA_OIF &&
                    RTA_PAYLOAD(attr) >= sizeof(index)) {
                    memcpy(&index, RTA_DATA(attr), sizeof(index));
                } else if (attr->rta_type == RTA_PRIORITY &&
                           RTA_PAYLOAD(attr) >= sizeof(priority)) {
                    memcpy(&priority, RTA_DATA(attr), sizeof(priority));
                }
            }
            if (index != 0u &&
                (best_index == 0u || priority < best_priority)) {
                best_priority = priority;
                best_index = index;
            }
        }
    }
    (void)close(fd);
    return H2_PAL_ERR_IO;
}

static h2_pal_netif_status_t *linux_snapshot_entry(
    linux_netif_snapshot_t *snapshot,
    const char *name,
    unsigned int flags) {
    for (size_t i = 0u; i < snapshot->count; ++i) {
        if (strncmp(snapshot->entries[i].ref.name, name,
                    H2_PAL_NETIF_NAME_MAX) == 0) {
            return &snapshot->entries[i];
        }
    }
    size_t length = strnlen(name, H2_PAL_NETIF_NAME_MAX);
    if (length == 0u || length >= H2_PAL_NETIF_NAME_MAX ||
        snapshot->count >= H2_LINUX_NETIF_SNAPSHOT_MAX) {
        return NULL;
    }
    h2_pal_netif_status_t *status = &snapshot->entries[snapshot->count++];
    memset(status, 0, sizeof(*status));
    status->ref.type = H2_PAL_NETIF_REF_NAME;
    status->ref.kind = linux_netif_kind(name, flags);
    memcpy(status->ref.name, name, length + 1u);
    status->kind = status->ref.kind;
    if ((flags & IFF_UP) != 0u) {
        status->flags |= H2_PAL_NETIF_FLAG_UP;
    }
    if ((flags & IFF_RUNNING) != 0u) {
        status->flags |= H2_PAL_NETIF_FLAG_LINK_UP;
    }
    if ((flags & IFF_POINTOPOINT) != 0u) {
        status->flags |= H2_PAL_NETIF_FLAG_POINT_TO_POINT;
    }
    return status;
}

static void linux_copy_address(
    h2_pal_net_addr_t *out,
    const struct sockaddr *address) {
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *ipv4 =
            (const struct sockaddr_in *)address;
        out->family = H2_PAL_NET_FAMILY_IPV4;
        memcpy(out->ip, &ipv4->sin_addr, 4u);
    } else if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 =
            (const struct sockaddr_in6 *)address;
        out->family = H2_PAL_NET_FAMILY_IPV6;
        memcpy(out->ip, &ipv6->sin6_addr, 16u);
    }
}

static void linux_fill_mtu(h2_pal_netif_status_t *status) {
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
    (void)close(fd);
}

static h2_pal_result_t linux_take_snapshot(
    linux_netif_snapshot_t *snapshot) {
#if defined(H2_LINUX_TESTING)
    if (s_linux_test_snapshot_enabled != 0) {
        *snapshot = s_linux_test_snapshot;
        return H2_PAL_OK;
    }
#endif
    memset(snapshot, 0, sizeof(*snapshot));
    char default_name[H2_PAL_NETIF_NAME_MAX] = {0};
    h2_pal_result_t default_rc = linux_default_name(default_name);
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
        h2_pal_netif_status_t *status = linux_snapshot_entry(
            snapshot, item->ifa_name, item->ifa_flags);
        if (status == NULL) {
            continue;
        }
        if (item->ifa_addr->sa_family == AF_INET) {
            linux_copy_address(&status->ipv4, item->ifa_addr);
            status->flags |= H2_PAL_NETIF_FLAG_HAS_IPV4;
            if (item->ifa_netmask != NULL) {
                linux_copy_address(&status->netmask4, item->ifa_netmask);
            }
        } else if (item->ifa_addr->sa_family == AF_INET6 &&
                   (status->flags & H2_PAL_NETIF_FLAG_HAS_IPV6) == 0u) {
            linux_copy_address(&status->ipv6, item->ifa_addr);
            status->flags |= H2_PAL_NETIF_FLAG_HAS_IPV6;
        } else if (item->ifa_addr->sa_family == AF_PACKET) {
            const struct sockaddr_ll *link =
                (const struct sockaddr_ll *)item->ifa_addr;
            if (link->sll_halen == sizeof(status->mac)) {
                memcpy(status->mac, link->sll_addr, sizeof(status->mac));
                status->mac_valid = 1u;
            }
        }
    }
    freeifaddrs(interfaces);
    for (size_t i = 0u; i < snapshot->count; ++i) {
        linux_fill_mtu(&snapshot->entries[i]);
        if (default_rc == H2_PAL_OK &&
            strncmp(snapshot->entries[i].ref.name, default_name,
                    H2_PAL_NETIF_NAME_MAX) == 0) {
            snapshot->entries[i].flags |= H2_PAL_NETIF_FLAG_DEFAULT_ROUTE;
        }
    }
    return H2_PAL_OK;
}
#endif

#if defined(__linux__)
static int linux_filter_matches(
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

static int linux_ref_matches(
    const h2_pal_netif_ref_t *ref,
    const h2_pal_netif_status_t *status) {
    if (ref->type == H2_PAL_NETIF_REF_NAME) {
        return memchr(ref->name, '\0', sizeof(ref->name)) != NULL &&
            strncmp(ref->name, status->ref.name,
                    H2_PAL_NETIF_NAME_MAX) == 0;
    }
    if (ref->type == H2_PAL_NETIF_REF_ID) {
        return ref->id != 0u &&
            ref->id == if_nametoindex(status->ref.name);
    }
    return ref->type == H2_PAL_NETIF_REF_KIND &&
        ref->kind == status->kind;
}
#endif

static h2_pal_result_t linux_netif_list(
    void *user,
    const h2_pal_netif_filter_t *filter,
    h2_pal_netif_list_fn on_netif,
    void *callback_user) {
    (void)user;
#if !defined(__linux__)
    (void)filter;
    (void)on_netif;
    (void)callback_user;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    linux_netif_snapshot_t snapshot;
    h2_pal_result_t rc = linux_take_snapshot(&snapshot);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (linux_filter_matches(filter, &snapshot.entries[i]) &&
            on_netif(callback_user, &snapshot.entries[i].ref,
                     &snapshot.entries[i]) != 0) {
            return H2_PAL_EXIT;
        }
    }
    return H2_PAL_OK;
#endif
}

static h2_pal_result_t linux_netif_find(
    void *user,
    const h2_pal_netif_filter_t *filter,
    h2_pal_netif_ref_t *out_ref) {
    (void)user;
#if !defined(__linux__)
    (void)filter;
    (void)out_ref;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    linux_netif_snapshot_t snapshot;
    h2_pal_result_t rc = linux_take_snapshot(&snapshot);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if (linux_filter_matches(filter, &snapshot.entries[i])) {
            *out_ref = snapshot.entries[i].ref;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
#endif
}

static h2_pal_result_t linux_netif_get_status(
    void *user,
    const h2_pal_netif_ref_t *ref,
    h2_pal_netif_status_t *out_status) {
    (void)user;
#if !defined(__linux__)
    (void)ref;
    (void)out_status;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    linux_netif_snapshot_t snapshot;
    h2_pal_result_t rc = linux_take_snapshot(&snapshot);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t i = 0u; i < snapshot.count; ++i) {
        if ((h2_pal_netif_ref_is_default(ref) &&
             (snapshot.entries[i].flags &
              H2_PAL_NETIF_FLAG_DEFAULT_ROUTE) != 0u) ||
            (ref != NULL && linux_ref_matches(ref, &snapshot.entries[i]))) {
            *out_status = snapshot.entries[i];
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
#endif
}

static h2_pal_result_t linux_netif_get_dns_servers(
    void *user,
    const h2_pal_netif_ref_t *ref,
    h2_pal_netif_dns_server_t *out_servers,
    size_t max_servers,
    size_t *out_count) {
    (void)user;
    *out_count = 0u;
#if !defined(__linux__)
    (void)ref;
    (void)out_servers;
    (void)max_servers;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    h2_pal_netif_status_t status;
    h2_pal_result_t rc = linux_netif_get_status(NULL, ref, &status);
    if (rc != H2_PAL_OK ||
        (status.flags & H2_PAL_NETIF_FLAG_DEFAULT_ROUTE) == 0u ||
        max_servers == 0u) {
        return rc;
    }
#if defined(H2_LINUX_TESTING)
    if (s_linux_test_snapshot_enabled != 0) {
        size_t count = s_linux_test_dns_count < max_servers ?
            s_linux_test_dns_count : max_servers;
        if (count > 0u) {
            memcpy(out_servers, s_linux_test_dns,
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
    while (*out_count < max_servers &&
           *out_count < H2_PAL_NETIF_DNS_MAX &&
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
    (void)fclose(resolver);
    return H2_PAL_OK;
#endif
}

static const h2_pal_netif_vtable_t s_linux_netif_vtable = {
    .list = linux_netif_list,
    .find = linux_netif_find,
    .get_status = linux_netif_get_status,
    .get_dns_servers = linux_netif_get_dns_servers,
};
static const h2_pal_netif_api_t s_linux_netif_api = {
    .user = NULL,
    .vtable = &s_linux_netif_vtable,
};

const h2_pal_netif_api_t *h2_linux_netif_api(void) {
    return &s_linux_netif_api;
}

struct h2_pal_system_event_subscription {
    int active;
    size_t in_flight;
    h2_pal_system_event_type_t type;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
};

typedef struct linux_event_dispatch {
    h2_pal_system_event_subscription_t *subscription;
    h2_pal_system_event_handler_t handler;
    void *handler_user;
} linux_event_dispatch_t;

static h2_pal_system_event_subscription_t
    s_linux_subscriptions[H2_LINUX_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
static pthread_mutex_t s_linux_event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_linux_event_idle = PTHREAD_COND_INITIALIZER;
static int s_linux_event_initialized;

typedef struct linux_netif_monitor {
    pthread_mutex_t mutex;
    pthread_t thread;
    h2_pal_netif_ref_t current;
    int current_valid;
    int running;
    int thread_started;
#if defined(__linux__)
    int route_fd;
    int wake_read_fd;
    int wake_write_fd;
#endif
} linux_netif_monitor_t;

#if defined(__linux__)
static linux_netif_monitor_t s_linux_monitor = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
#if defined(__linux__)
    .route_fd = -1,
    .wake_read_fd = -1,
    .wake_write_fd = -1,
#endif
};
#endif

static int linux_system_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms);

#if defined(__linux__)
static h2_pal_result_t linux_default_ref(
    h2_pal_netif_ref_t *out_ref,
    int *out_valid) {
    char name[H2_PAL_NETIF_NAME_MAX] = {0};
    h2_pal_result_t rc = linux_default_name(name);
    memset(out_ref, 0, sizeof(*out_ref));
    *out_valid = 0;
#if defined(H2_LINUX_TESTING)
    if (s_linux_test_snapshot_enabled != 0) {
        for (size_t i = 0u; i < s_linux_test_snapshot.count; ++i) {
            if ((s_linux_test_snapshot.entries[i].flags &
                 H2_PAL_NETIF_FLAG_DEFAULT_ROUTE) != 0u) {
                *out_ref = s_linux_test_snapshot.entries[i].ref;
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
    out_ref->kind = linux_netif_kind(name, 0u);
    memcpy(out_ref->name, name, strnlen(name, sizeof(out_ref->name)) + 1u);
    *out_valid = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t linux_monitor_reconcile_ref(
    const h2_pal_netif_ref_t *next,
    int next_valid) {
    h2_pal_netif_default_changed_t change;
    memset(&change, 0, sizeof(change));
    (void)pthread_mutex_lock(&s_linux_monitor.mutex);
    int changed = next_valid != s_linux_monitor.current_valid ||
        (next_valid != 0 && !h2_pal_netif_ref_equal(
            next, &s_linux_monitor.current));
    if (changed != 0) {
        change.previous_valid =
            (uint8_t)s_linux_monitor.current_valid;
        change.current_valid = (uint8_t)next_valid;
        if (s_linux_monitor.current_valid != 0) {
            change.previous = s_linux_monitor.current;
        }
        if (next_valid != 0) {
            change.current = *next;
        }
        s_linux_monitor.current = *next;
        s_linux_monitor.current_valid = next_valid;
    }
    (void)pthread_mutex_unlock(&s_linux_monitor.mutex);
    if (changed != 0) {
        h2_pal_system_event_t event = {
            .type = H2_PAL_SYSTEM_EVENT_TYPE_NETIF_DEFAULT_CHANGED,
            .payload = &change,
            .payload_size = sizeof(change),
        };
        return linux_system_event_post(NULL, &event, 0u);
    }
    return H2_PAL_OK;
}

static void linux_monitor_reconcile(void) {
    h2_pal_netif_ref_t next;
    int next_valid = 0;
    h2_pal_result_t rc = linux_default_ref(&next, &next_valid);
    if (rc != H2_PAL_OK) {
        (void)fprintf(stderr,
                      "H2_LOG level=warn scope=linux_netif "
                      "message=default-route-query-failed result=%d\n",
                      rc);
        return;
    }
    rc = linux_monitor_reconcile_ref(&next, next_valid);
    if (rc != H2_PAL_OK) {
        (void)fprintf(stderr,
                      "H2_LOG level=warn scope=linux_netif "
                      "message=default-route-event-failed result=%d\n",
                      rc);
    }
}

static void *linux_monitor_thread(void *user) {
    (void)user;
    struct pollfd descriptors[2] = {
        {.fd = s_linux_monitor.route_fd, .events = POLLIN},
        {.fd = s_linux_monitor.wake_read_fd, .events = POLLIN},
    };
    for (;;) {
        int rc;
        do {
            rc = poll(descriptors, 2u, -1);
        } while (rc < 0 && errno == EINTR);
        (void)pthread_mutex_lock(&s_linux_monitor.mutex);
        int running = s_linux_monitor.running;
        (void)pthread_mutex_unlock(&s_linux_monitor.mutex);
        if (running == 0 || rc <= 0 ||
            (descriptors[1].revents & POLLIN) != 0) {
            break;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            uint8_t message[8192];
            if (recv(s_linux_monitor.route_fd, message,
                     sizeof(message), 0) > 0) {
                linux_monitor_reconcile();
            }
        }
    }
    return NULL;
}
#endif

static h2_pal_result_t linux_monitor_start(void) {
#if !defined(__linux__)
    return H2_PAL_ERR_UNSUPPORTED;
#else
    (void)pthread_mutex_lock(&s_linux_monitor.mutex);
    if (s_linux_monitor.running != 0) {
        (void)pthread_mutex_unlock(&s_linux_monitor.mutex);
        return H2_PAL_OK;
    }
    h2_pal_result_t rc = linux_default_ref(
        &s_linux_monitor.current,
        &s_linux_monitor.current_valid);
#if defined(H2_LINUX_TESTING)
    if (rc == H2_PAL_OK && s_linux_test_snapshot_enabled != 0) {
        s_linux_monitor.running = 1;
        (void)pthread_mutex_unlock(&s_linux_monitor.mutex);
        return H2_PAL_OK;
    }
#endif
    int wake_pipe[2] = {-1, -1};
    if (rc == H2_PAL_OK && pipe(wake_pipe) != 0) {
        rc = H2_PAL_ERR_IO;
    }
    if (rc == H2_PAL_OK) {
        rc = linux_route_socket(
            RTMGRP_IPV4_ROUTE | RTMGRP_LINK | RTMGRP_IPV4_IFADDR,
            &s_linux_monitor.route_fd);
    }
    if (rc == H2_PAL_OK) {
        s_linux_monitor.wake_read_fd = wake_pipe[0];
        s_linux_monitor.wake_write_fd = wake_pipe[1];
        s_linux_monitor.running = 1;
        if (pthread_create(&s_linux_monitor.thread, NULL,
                           linux_monitor_thread, NULL) != 0) {
            s_linux_monitor.running = 0;
            rc = H2_PAL_ERR_IO;
        } else {
            s_linux_monitor.thread_started = 1;
        }
    }
    (void)pthread_mutex_unlock(&s_linux_monitor.mutex);
    if (rc != H2_PAL_OK) {
        if (s_linux_monitor.route_fd >= 0) {
            (void)close(s_linux_monitor.route_fd);
        }
        if (wake_pipe[0] >= 0) {
            (void)close(wake_pipe[0]);
            (void)close(wake_pipe[1]);
        }
        s_linux_monitor.route_fd = -1;
    }
    return rc;
#endif
}

static void linux_monitor_stop(void) {
#if defined(__linux__)
    (void)pthread_mutex_lock(&s_linux_monitor.mutex);
    int join = s_linux_monitor.thread_started;
    s_linux_monitor.running = 0;
    int wake_fd = s_linux_monitor.wake_write_fd;
    (void)pthread_mutex_unlock(&s_linux_monitor.mutex);
    if (wake_fd >= 0) {
        const uint8_t wake = 1u;
        ssize_t written = write(wake_fd, &wake, sizeof(wake));
        (void)written;
    }
    if (join != 0) {
        (void)pthread_join(s_linux_monitor.thread, NULL);
    }
    if (s_linux_monitor.route_fd >= 0) {
        (void)close(s_linux_monitor.route_fd);
    }
    if (s_linux_monitor.wake_read_fd >= 0) {
        (void)close(s_linux_monitor.wake_read_fd);
    }
    if (s_linux_monitor.wake_write_fd >= 0) {
        (void)close(s_linux_monitor.wake_write_fd);
    }
    (void)pthread_mutex_lock(&s_linux_monitor.mutex);
    s_linux_monitor.route_fd = -1;
    s_linux_monitor.wake_read_fd = -1;
    s_linux_monitor.wake_write_fd = -1;
    s_linux_monitor.thread_started = 0;
    s_linux_monitor.current_valid = 0;
    memset(&s_linux_monitor.current, 0,
           sizeof(s_linux_monitor.current));
    (void)pthread_mutex_unlock(&s_linux_monitor.mutex);
#endif
}

#if defined(__linux__) && defined(H2_LINUX_TESTING)
void h2_linux_netif_test_set_snapshot(
    const h2_pal_netif_status_t *entries,
    size_t count,
    const h2_pal_netif_dns_server_t *dns,
    size_t dns_count) {
    assert(count <= H2_LINUX_NETIF_SNAPSHOT_MAX);
    assert(dns_count <= H2_PAL_NETIF_DNS_MAX);
    memset(&s_linux_test_snapshot, 0,
           sizeof(s_linux_test_snapshot));
    if (count > 0u) {
        memcpy(s_linux_test_snapshot.entries, entries,
               count * sizeof(entries[0]));
    }
    s_linux_test_snapshot.count = count;
    memset(s_linux_test_dns, 0, sizeof(s_linux_test_dns));
    if (dns_count > 0u) {
        memcpy(s_linux_test_dns, dns,
               dns_count * sizeof(dns[0]));
    }
    s_linux_test_dns_count = dns_count;
    s_linux_test_snapshot_enabled = 1;
}

void h2_linux_netif_test_set_default(
    const h2_pal_netif_ref_t *ref,
    int valid) {
    (void)pthread_mutex_lock(&s_linux_monitor.mutex);
    memset(&s_linux_monitor.current, 0,
           sizeof(s_linux_monitor.current));
    if (valid != 0 && ref != NULL) {
        s_linux_monitor.current = *ref;
        s_linux_monitor.current_valid = 1;
    } else {
        s_linux_monitor.current_valid = 0;
    }
    (void)pthread_mutex_unlock(&s_linux_monitor.mutex);
}

h2_pal_result_t h2_linux_netif_test_reconcile_default(
    const h2_pal_netif_ref_t *ref,
    int valid) {
    h2_pal_netif_ref_t zero;
    memset(&zero, 0, sizeof(zero));
    return linux_monitor_reconcile_ref(
        valid != 0 && ref != NULL ? ref : &zero,
        valid != 0 && ref != NULL);
}
#endif

static int linux_system_event_init(void *user) {
    (void)user;
    (void)pthread_mutex_lock(&s_linux_event_mutex);
    if (s_linux_event_initialized != 0) {
        (void)pthread_mutex_unlock(&s_linux_event_mutex);
        return H2_PAL_OK;
    }
    s_linux_event_initialized = 1;
    (void)pthread_mutex_unlock(&s_linux_event_mutex);
    h2_pal_result_t rc = linux_monitor_start();
    if (rc != H2_PAL_OK) {
        (void)pthread_mutex_lock(&s_linux_event_mutex);
        s_linux_event_initialized = 0;
        (void)pthread_mutex_unlock(&s_linux_event_mutex);
    }
    return rc;
}

static void linux_system_event_deinit(void *user) {
    (void)user;
    linux_monitor_stop();
    (void)pthread_mutex_lock(&s_linux_event_mutex);
    s_linux_event_initialized = 0;
    for (size_t i = 0u;
         i < H2_LINUX_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        s_linux_subscriptions[i].active = 0;
        while (s_linux_subscriptions[i].in_flight != 0u) {
            (void)pthread_cond_wait(
                &s_linux_event_idle, &s_linux_event_mutex);
        }
    }
    memset(s_linux_subscriptions, 0,
           sizeof(s_linux_subscriptions));
    (void)pthread_mutex_unlock(&s_linux_event_mutex);
}

static int linux_system_event_post(
    void *user,
    const h2_pal_system_event_t *event,
    uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    int rc = h2_pal_system_event_validate(event);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    linux_event_dispatch_t dispatches[
        H2_LINUX_SYSTEM_EVENT_MAX_SUBSCRIPTIONS];
    size_t count = 0u;
    (void)pthread_mutex_lock(&s_linux_event_mutex);
    if (s_linux_event_initialized == 0) {
        (void)pthread_mutex_unlock(&s_linux_event_mutex);
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t i = 0u;
         i < H2_LINUX_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *subscription =
            &s_linux_subscriptions[i];
        if (subscription->active != 0 &&
            subscription->type == event->type &&
            subscription->handler != NULL) {
            ++subscription->in_flight;
            dispatches[count++] = (linux_event_dispatch_t){
                .subscription = subscription,
                .handler = subscription->handler,
                .handler_user = subscription->handler_user,
            };
        }
    }
    (void)pthread_mutex_unlock(&s_linux_event_mutex);

    int result = H2_PAL_OK;
    for (size_t i = 0u; i < count; ++i) {
        int handler_rc = dispatches[i].handler(
            dispatches[i].handler_user, event);
        (void)pthread_mutex_lock(&s_linux_event_mutex);
        --dispatches[i].subscription->in_flight;
        (void)pthread_cond_broadcast(&s_linux_event_idle);
        (void)pthread_mutex_unlock(&s_linux_event_mutex);
        if (result == H2_PAL_OK && handler_rc != H2_PAL_OK) {
            result = handler_rc;
        }
    }
    return result;
}

static int linux_system_event_subscribe(
    void *user,
    h2_pal_system_event_type_t type,
    h2_pal_system_event_handler_t handler,
    void *handler_user,
    h2_pal_system_event_subscription_t **out_subscription) {
    (void)user;
    if (type <= H2_PAL_SYSTEM_EVENT_TYPE_NONE ||
        type >= H2_PAL_SYSTEM_EVENT_TYPE_COUNT || handler == NULL ||
        out_subscription == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_subscription = NULL;
    (void)pthread_mutex_lock(&s_linux_event_mutex);
    if (s_linux_event_initialized == 0) {
        (void)pthread_mutex_unlock(&s_linux_event_mutex);
        return H2_PAL_ERR_INVALID_STATE;
    }
    for (size_t i = 0u;
         i < H2_LINUX_SYSTEM_EVENT_MAX_SUBSCRIPTIONS; ++i) {
        h2_pal_system_event_subscription_t *subscription =
            &s_linux_subscriptions[i];
        if (subscription->active == 0 && subscription->in_flight == 0u) {
            subscription->active = 1;
            subscription->type = type;
            subscription->handler = handler;
            subscription->handler_user = handler_user;
            *out_subscription = subscription;
            (void)pthread_mutex_unlock(&s_linux_event_mutex);
            return H2_PAL_OK;
        }
    }
    (void)pthread_mutex_unlock(&s_linux_event_mutex);
    return H2_PAL_ERR_FULL;
}

static void linux_system_event_unsubscribe(
    void *user,
    h2_pal_system_event_subscription_t *subscription) {
    (void)user;
    if (subscription == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&s_linux_event_mutex);
    subscription->active = 0;
    while (subscription->in_flight != 0u) {
        (void)pthread_cond_wait(
            &s_linux_event_idle, &s_linux_event_mutex);
    }
    memset(subscription, 0, sizeof(*subscription));
    (void)pthread_mutex_unlock(&s_linux_event_mutex);
}

static const h2_pal_system_event_vtable_t s_linux_event_vtable = {
    .init = linux_system_event_init,
    .deinit = linux_system_event_deinit,
    .post = linux_system_event_post,
    .subscribe = linux_system_event_subscribe,
    .unsubscribe = linux_system_event_unsubscribe,
};
static const h2_pal_system_event_api_t s_linux_event_api = {
    .user = NULL,
    .vtable = &s_linux_event_vtable,
};

const h2_pal_system_event_api_t *h2_linux_system_event_api(void) {
    return &s_linux_event_api;
}

typedef struct linux_display_state {
    h2_linux_display_config_t config;
#if !defined(H2_LINUX_NO_FB)
    int native_rgb565;
    int native_argb8888;
    int fd;
    uint8_t *memory;
    size_t memory_size;
    uint32_t draw_yoffset;
    int double_buffered;
    int draw_page_seeded;
    struct fb_fix_screeninfo fixed;
    struct fb_var_screeninfo variable;
#endif
} linux_display_state_t;

static linux_display_state_t s_display = {
    .config = {.device_path = "/dev/fb0", .width = 1024u, .height = 600u},
#if !defined(H2_LINUX_NO_FB)
    .fd = -1,
#endif
};

static int display_back_page_yoffset(
    uint32_t visible_height,
    uint32_t virtual_height,
    uint32_t current_yoffset,
    uint32_t line_length,
    size_t memory_size,
    uint32_t *out_yoffset) {
    if (visible_height == 0u || line_length == 0u || out_yoffset == NULL ||
        virtual_height / visible_height < 2u ||
        memory_size / line_length / visible_height < 2u ||
        current_yoffset % visible_height != 0u ||
        current_yoffset > virtual_height - visible_height) {
        return 0;
    }
    *out_yoffset = current_yoffset < visible_height ? visible_height : 0u;
    return 1;
}

static int display_copy_page(
    uint8_t *memory,
    size_t memory_size,
    uint32_t line_length,
    uint32_t page_height,
    uint32_t source_yoffset,
    uint32_t destination_yoffset) {
    if (memory == NULL || line_length == 0u || page_height == 0u) return 0;
    const size_t available_lines = memory_size / line_length;
    if ((size_t)page_height > available_lines ||
        (size_t)source_yoffset > available_lines - page_height ||
        (size_t)destination_yoffset > available_lines - page_height) {
        return 0;
    }
    const size_t page_size = (size_t)line_length * page_height;
    memmove(
        memory + (size_t)destination_yoffset * line_length,
        memory + (size_t)source_yoffset * line_length,
        page_size);
    return 1;
}

#if defined(H2_LINUX_TESTING)
int h2_linux_display_test_back_page_yoffset(
    uint32_t visible_height,
    uint32_t virtual_height,
    uint32_t current_yoffset,
    uint32_t line_length,
    size_t memory_size,
    uint32_t *out_yoffset) {
    return display_back_page_yoffset(
        visible_height,
        virtual_height,
        current_yoffset,
        line_length,
        memory_size,
        out_yoffset);
}

int h2_linux_display_test_copy_page(
    uint8_t *memory,
    size_t memory_size,
    uint32_t line_length,
    uint32_t page_height,
    uint32_t source_yoffset,
    uint32_t destination_yoffset) {
    return display_copy_page(
        memory,
        memory_size,
        line_length,
        page_height,
        source_yoffset,
        destination_yoffset);
}

#if !defined(H2_LINUX_NO_FB)
int h2_linux_display_test_use_framebuffer(
    uint8_t *memory,
    size_t memory_size,
    uint32_t width,
    uint32_t height,
    uint32_t line_length) {
    uint32_t draw_yoffset = 0u;
    if (memory == NULL || memory_size > UINT32_MAX || width == 0u || height == 0u ||
        height > UINT32_MAX / 2u ||
        line_length < (size_t)width * sizeof(uint16_t) ||
        !display_back_page_yoffset(
            height,
            height * 2u,
            0u,
            line_length,
            memory_size,
            &draw_yoffset)) {
        return 0;
    }
    memset(&s_display, 0, sizeof(s_display));
    s_display.config.width = width;
    s_display.config.height = height;
    s_display.native_rgb565 = 1;
    s_display.fd = 0;
    s_display.memory = memory;
    s_display.memory_size = memory_size;
    s_display.draw_yoffset = draw_yoffset;
    s_display.double_buffered = 1;
    s_display.fixed.line_length = line_length;
    s_display.fixed.smem_len = memory_size;
    s_display.variable.xres = width;
    s_display.variable.xres_virtual = width;
    s_display.variable.yres = height;
    s_display.variable.yres_virtual = height * 2u;
    s_display.variable.bits_per_pixel = 16u;
    return 1;
}
#endif
#endif

h2_pal_result_t h2_linux_configure_display(
    const h2_linux_display_config_t *config) {
    if (config == NULL || config->device_path == NULL || config->device_path[0] == '\0' ||
        config->width == 0u || config->height == 0u || config->width > INT_MAX ||
        config->height > INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
#if !defined(H2_LINUX_NO_FB)
    if (s_display.fd >= 0) return H2_PAL_ERR_INVALID_STATE;
#endif
    s_display.config = *config;
    return H2_PAL_OK;
}

static int display_open(void *user) {
    linux_display_state_t *display = user;
#if defined(H2_LINUX_NO_FB)
    (void)display;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    if (display->fd >= 0) return H2_PAL_OK;
    display->fd = open(display->config.device_path, O_RDWR | O_CLOEXEC);
    if (display->fd < 0) return H2_PAL_ERR_IO;
    if (ioctl(display->fd, FBIOGET_FSCREENINFO, &display->fixed) != 0 ||
        ioctl(display->fd, FBIOGET_VSCREENINFO, &display->variable) != 0) {
        (void)close(display->fd);
        display->fd = -1;
        return H2_PAL_ERR_IO;
    }
    display->native_rgb565 =
        display->variable.bits_per_pixel == 16u &&
        display->variable.red.offset == 11u && display->variable.red.length == 5u &&
        display->variable.red.msb_right == 0u &&
        display->variable.green.offset == 5u && display->variable.green.length == 6u &&
        display->variable.green.msb_right == 0u &&
        display->variable.blue.offset == 0u && display->variable.blue.length == 5u &&
        display->variable.blue.msb_right == 0u && display->variable.transp.length == 0u;
    display->native_argb8888 =
        display->variable.bits_per_pixel == 32u &&
        display->variable.red.offset == 16u && display->variable.red.length == 8u &&
        display->variable.red.msb_right == 0u &&
        display->variable.green.offset == 8u && display->variable.green.length == 8u &&
        display->variable.green.msb_right == 0u &&
        display->variable.blue.offset == 0u && display->variable.blue.length == 8u &&
        display->variable.blue.msb_right == 0u &&
        display->variable.transp.offset == 24u && display->variable.transp.length == 8u &&
        display->variable.transp.msb_right == 0u;
    const size_t native_pixel_size = display->variable.bits_per_pixel / 8u;
    if (display->fixed.type != FB_TYPE_PACKED_PIXELS ||
        display->fixed.visual != FB_VISUAL_TRUECOLOR || display->variable.nonstd != 0u ||
        (!display->native_rgb565 && !display->native_argb8888) ||
        display->variable.xres < display->config.width ||
        display->variable.yres < display->config.height ||
        display->variable.xoffset != 0u ||
        display->variable.yoffset + display->config.height >
            display->variable.yres_virtual ||
        display->fixed.line_length < display->config.width * native_pixel_size ||
        display->fixed.smem_len <
            (size_t)display->fixed.line_length * display->config.height) {
        (void)close(display->fd);
        display->fd = -1;
        return H2_PAL_ERR_FORMAT;
    }
    display->draw_yoffset = display->variable.yoffset;
    display->double_buffered = display_back_page_yoffset(
        display->variable.yres,
        display->variable.yres_virtual,
        display->variable.yoffset,
        display->fixed.line_length,
        display->fixed.smem_len,
        &display->draw_yoffset);
    display->memory_size = display->fixed.smem_len;
    display->memory = mmap(NULL, display->memory_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                           display->fd, 0);
    if (display->memory == MAP_FAILED) {
        display->memory = NULL;
        (void)close(display->fd);
        display->fd = -1;
        return H2_PAL_ERR_IO;
    }
    display->draw_page_seeded = 0;
    return H2_PAL_OK;
#endif
}

static uint32_t display_rgb565_to_argb8888(uint16_t pixel) {
    const uint32_t red = (uint32_t)((pixel >> 11u) & 0x1fu);
    const uint32_t green = (uint32_t)((pixel >> 5u) & 0x3fu);
    const uint32_t blue = (uint32_t)(pixel & 0x1fu);
    return UINT32_C(0xff000000) |
        ((red << 3u | red >> 2u) << 16u) |
        ((green << 2u | green >> 4u) << 8u) |
        (blue << 3u | blue >> 2u);
}

#if defined(H2_LINUX_TESTING)
uint32_t h2_linux_display_test_rgb565_to_argb8888(uint16_t pixel) {
    return display_rgb565_to_argb8888(pixel);
}
#endif

static int display_get_info(void *user, h2_display_info_t *out_info) {
    linux_display_state_t *display = user;
    *out_info = (h2_display_info_t){
        .width = (int)display->config.width,
        .height = (int)display->config.height,
        .native_format = H2_DISPLAY_PIXEL_RGB565,
    };
    return H2_PAL_OK;
}

static int display_draw_bitmap(
    void *user,
    const h2_display_rect_t *rect,
    const void *pixels,
    size_t stride_bytes,
    h2_display_pixel_format_t format) {
    linux_display_state_t *display = user;
#if defined(H2_LINUX_NO_FB)
    (void)display;
    (void)rect;
    (void)pixels;
    (void)stride_bytes;
    (void)format;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    if (display->fd < 0 || display->memory == NULL) return H2_PAL_ERR_INVALID_STATE;
    if (format != H2_DISPLAY_PIXEL_RGB565 || rect->x < 0 || rect->y < 0 ||
        rect->width > (int)display->config.width ||
        rect->height > (int)display->config.height ||
        rect->x > (int)display->config.width - rect->width ||
        rect->y > (int)display->config.height - rect->height ||
        stride_bytes < (size_t)rect->width * sizeof(uint16_t)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const int full_scanout =
        rect->x == 0 && rect->y == 0 &&
        rect->width == (int)display->config.width &&
        rect->height == (int)display->config.height &&
        display->config.width == display->variable.xres &&
        display->config.height == display->variable.yres;
    if (display->double_buffered && !display->draw_page_seeded) {
        if (!full_scanout &&
            !display_copy_page(
                display->memory,
                display->memory_size,
                display->fixed.line_length,
                display->variable.yres,
                display->variable.yoffset,
                display->draw_yoffset)) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        display->draw_page_seeded = 1;
    }
    for (int row = 0; row < rect->height; ++row) {
        uint8_t *destination = display->memory +
            ((size_t)rect->y + (size_t)row + display->draw_yoffset) * display->fixed.line_length +
            ((size_t)rect->x + display->variable.xoffset) *
                (display->native_rgb565 ? sizeof(uint16_t) : sizeof(uint32_t));
        const uint8_t *source = (const uint8_t *)pixels + (size_t)row * stride_bytes;
        if (display->native_rgb565) {
            memcpy(destination, source, (size_t)rect->width * sizeof(uint16_t));
        } else {
            for (int column = 0; column < rect->width; ++column) {
                uint16_t pixel;
                uint32_t native_pixel;
                memcpy(&pixel, source + (size_t)column * sizeof(pixel), sizeof(pixel));
                native_pixel = display_rgb565_to_argb8888(pixel);
                memcpy(
                    destination + (size_t)column * sizeof(native_pixel),
                    &native_pixel,
                    sizeof(native_pixel));
            }
        }
    }
    return H2_PAL_OK;
#endif
}

static int display_present(void *user) {
#if defined(H2_LINUX_NO_FB)
    (void)user;
    return H2_PAL_ERR_UNSUPPORTED;
#else
    linux_display_state_t *display = user;
    if (display->fd < 0) return H2_PAL_ERR_INVALID_STATE;
    if (!display->double_buffered) return H2_PAL_OK;
    if (!display->draw_page_seeded &&
        !display_copy_page(
            display->memory,
            display->memory_size,
            display->fixed.line_length,
            display->variable.yres,
            display->variable.yoffset,
            display->draw_yoffset)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    struct fb_var_screeninfo next = display->variable;
    next.yoffset = display->draw_yoffset;
    next.activate = FB_ACTIVATE_VBL;
#if defined(H2_LINUX_TESTING)
    (void)display->fd;
#else
    if (ioctl(display->fd, FBIOPAN_DISPLAY, &next) != 0) {
        return H2_PAL_ERR_IO;
    }
#endif
    display->draw_yoffset = display->variable.yoffset;
    display->variable = next;
    display->draw_page_seeded = 0;
    return H2_PAL_OK;
#endif
}

static int display_brightness(void *user, uint32_t percent) {
    (void)user;
    (void)percent;
    return H2_PAL_ERR_UNSUPPORTED;
}

static int display_close(void *user) {
    linux_display_state_t *display = user;
#if defined(H2_LINUX_NO_FB)
    (void)display;
#else
    if (display->memory != NULL) {
        (void)munmap(display->memory, display->memory_size);
        display->memory = NULL;
        display->memory_size = 0u;
        display->draw_yoffset = 0u;
        display->double_buffered = 0;
        display->draw_page_seeded = 0;
    }
    if (display->fd >= 0) {
        (void)close(display->fd);
        display->fd = -1;
    }
#endif
    return H2_PAL_OK;
}

static const h2_pal_display_vtable_t s_display_vtable = {
    .open = display_open,
    .get_info = display_get_info,
    .draw_bitmap = display_draw_bitmap,
    .present = display_present,
    .set_brightness_percent = display_brightness,
    .close = display_close,
};
static const h2_pal_display_api_t s_display_api = {
    .user = &s_display,
    .vtable = &s_display_vtable,
};

const h2_pal_display_api_t *h2_linux_display_api(void) {
    return &s_display_api;
}
