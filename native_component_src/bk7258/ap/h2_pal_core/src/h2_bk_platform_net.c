#include "h2_bk_platform_core.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/time.h>

#include <components/netif.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <os/os.h>

#define H2_BK_NET_TLS_SOCKET_MAX 8u
#define H2_BK_NET_TLS_ALPN_MAX 2u
#define H2_BK_NET_TLS_ALPN_LEN 16u
#define H2_BK_NET_RESOLVER_MAX 8u

typedef enum bk_net_tls_socket_state {
    BK_NET_TLS_SOCKET_FREE = 0,
    BK_NET_TLS_SOCKET_ACTIVE,
    BK_NET_TLS_SOCKET_CONFIGURING,
    BK_NET_TLS_SOCKET_CLOSING,
} bk_net_tls_socket_state_t;

typedef struct bk_net_tls_socket {
    bk_net_tls_socket_state_t state;
    h2_pal_net_socket_t fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    char alpn_storage[H2_BK_NET_TLS_ALPN_MAX][H2_BK_NET_TLS_ALPN_LEN];
    const char *alpn[H2_BK_NET_TLS_ALPN_MAX + 1u];
} bk_net_tls_socket_t;

typedef struct bk_net_resolver {
    beken_mutex_t lock;
    beken_semaphore_t done;
    int completed;
    int closed;
    h2_pal_result_t result;
    h2_pal_net_addr_t addr;
    char host[];
} bk_net_resolver_t;

static bk_net_tls_socket_t s_bk_tls_sockets[H2_BK_NET_TLS_SOCKET_MAX];
static beken_mutex_t s_bk_tls_mutex;
static int s_bk_tls_mutex_ready;
static beken_mutex_t s_bk_tls_io_mutexes[H2_BK_NET_TLS_SOCKET_MAX];
static uint32_t s_bk_tls_generations[H2_BK_NET_TLS_SOCKET_MAX];
static size_t s_bk_resolver_count;

static int wait_fd(int fd, int write_ready, uint32_t timeout_ms);
static uint64_t bk_net_now_ms(void);
static uint32_t bk_net_timeout_remaining_ms(uint64_t deadline_ms);

static void bk_net_tls_init(void) {
    if (!s_bk_tls_mutex_ready &&
        rtos_init_mutex(&s_bk_tls_mutex) == kNoErr) {
        size_t index = 0u;
        for (; index < H2_BK_NET_TLS_SOCKET_MAX; ++index) {
            s_bk_tls_sockets[index].fd = -1;
            if (rtos_init_mutex(&s_bk_tls_io_mutexes[index]) != kNoErr) {
                break;
            }
        }
        if (index == H2_BK_NET_TLS_SOCKET_MAX) {
            s_bk_tls_mutex_ready = 1;
        } else {
            while (index > 0u) {
                index -= 1u;
                (void)rtos_deinit_mutex(&s_bk_tls_io_mutexes[index]);
            }
            (void)rtos_deinit_mutex(&s_bk_tls_mutex);
        }
    }
}

static int bk_net_tls_find(h2_pal_net_socket_t fd) {
    for (size_t index = 0u; index < H2_BK_NET_TLS_SOCKET_MAX; ++index) {
        if (s_bk_tls_sockets[index].state != BK_NET_TLS_SOCKET_FREE &&
            s_bk_tls_sockets[index].fd == fd) {
            return (int)index;
        }
    }
    return -1;
}

static int bk_net_tls_find_free(void) {
    for (size_t index = 0u; index < H2_BK_NET_TLS_SOCKET_MAX; ++index) {
        if (s_bk_tls_sockets[index].state == BK_NET_TLS_SOCKET_FREE) {
            return (int)index;
        }
    }
    return -1;
}

static int bk_net_tls_take_mutex(
    beken_mutex_t *mutex,
    int bounded,
    uint32_t timeout_ms,
    uint64_t deadline_ms) {
    if (mutex == NULL || *mutex == NULL) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    if (!bounded) {
        return rtos_lock_mutex(mutex) == kNoErr
            ? H2_PAL_OK
            : H2_PAL_ERR_UNAVAILABLE;
    }
    uint32_t remaining_ms = timeout_ms == 0u
        ? 0u
        : bk_net_timeout_remaining_ms(deadline_ms);
    if (timeout_ms != 0u && remaining_ms == 0u) {
        return H2_PAL_ERR_TIMEOUT;
    }
    bk_err_t result = remaining_ms == 0u
        ? rtos_trylock_mutex(mutex)
        : rtos_lock_mutex_timeout(mutex, remaining_ms);
    if (result == kNoErr) {
        return H2_PAL_OK;
    }
    return timeout_ms == 0u
        ? H2_PAL_ERR_WOULD_BLOCK
        : H2_PAL_ERR_TIMEOUT;
}

static int bk_net_tls_acquire(
    h2_pal_net_socket_t fd,
    int bounded,
    uint32_t timeout_ms,
    uint64_t deadline_ms,
    bk_net_tls_socket_t **out_socket,
    size_t *out_index) {
    if (!s_bk_tls_mutex_ready) {
        return 0;
    }
    int rc = bk_net_tls_take_mutex(
        &s_bk_tls_mutex, bounded, timeout_ms, deadline_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    int slot_index = bk_net_tls_find(fd);
    if (slot_index < 0) {
        (void)rtos_unlock_mutex(&s_bk_tls_mutex);
        return 0;
    }
    bk_net_tls_socket_t *slot = &s_bk_tls_sockets[slot_index];
    if (slot->state != BK_NET_TLS_SOCKET_ACTIVE) {
        (void)rtos_unlock_mutex(&s_bk_tls_mutex);
        return H2_PAL_ERR_CLOSED;
    }
    uint32_t generation = s_bk_tls_generations[slot_index];
    (void)rtos_unlock_mutex(&s_bk_tls_mutex);

    rc = bk_net_tls_take_mutex(
        &s_bk_tls_io_mutexes[slot_index], bounded, timeout_ms, deadline_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = bk_net_tls_take_mutex(
        &s_bk_tls_mutex, bounded, timeout_ms, deadline_ms);
    if (rc != H2_PAL_OK) {
        (void)rtos_unlock_mutex(&s_bk_tls_io_mutexes[slot_index]);
        return rc;
    }
    if (slot->state != BK_NET_TLS_SOCKET_ACTIVE || slot->fd != fd ||
        s_bk_tls_generations[slot_index] != generation) {
        (void)rtos_unlock_mutex(&s_bk_tls_mutex);
        (void)rtos_unlock_mutex(&s_bk_tls_io_mutexes[slot_index]);
        return H2_PAL_ERR_CLOSED;
    }
    (void)rtos_unlock_mutex(&s_bk_tls_mutex);
    *out_socket = slot;
    *out_index = (size_t)slot_index;
    return 1;
}

static void bk_net_tls_release(size_t slot_index) {
    (void)rtos_unlock_mutex(&s_bk_tls_io_mutexes[slot_index]);
}

static int bk_net_tls_random(void *user, unsigned char *out, size_t len) {
    (void)user;
    return h2_pal_crypto_random(h2_bk_platform_crypto_api(), out, len) == H2_PAL_OK
        ? 0
        : -1;
}

static int bk_net_tls_send_raw(
    void *user, const unsigned char *data, size_t len) {
    bk_net_tls_socket_t *socket = (bk_net_tls_socket_t *)user;
    int sent = send(socket->fd, data, len, MSG_DONTWAIT);
    if (sent >= 0) {
        return sent;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
        ? MBEDTLS_ERR_SSL_WANT_WRITE
        : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bk_net_tls_recv_raw(
    void *user, unsigned char *data, size_t len) {
    bk_net_tls_socket_t *socket = (bk_net_tls_socket_t *)user;
    int received = recv(socket->fd, data, len, MSG_DONTWAIT);
    if (received >= 0) {
        return received;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
        ? MBEDTLS_ERR_SSL_WANT_READ
        : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static void bk_net_tls_free(bk_net_tls_socket_t *socket) {
    if (socket == NULL || socket->state == BK_NET_TLS_SOCKET_FREE) {
        return;
    }
    mbedtls_ssl_free(&socket->ssl);
    mbedtls_ssl_config_free(&socket->config);
    mbedtls_x509_crt_free(&socket->ca);
    memset(socket, 0, sizeof(*socket));
    socket->fd = -1;
}

static int bk_net_tls_wait(
    bk_net_tls_socket_t *socket, int result, uint32_t timeout_ms) {
    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        return wait_fd(socket->fd, 0, timeout_ms);
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return wait_fd(socket->fd, 1, timeout_ms);
    }
    return H2_PAL_ERR_IO;
}

static h2_pal_result_t bk_net_tls_copy_alpn(
    bk_net_tls_socket_t *socket,
    const h2_pal_net_tls_config_t *config) {
    if (config->alpn == NULL || config->alpn[0] == '\0') {
        return H2_PAL_OK;
    }
    if (strlen(config->alpn) >= H2_BK_NET_TLS_ALPN_LEN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)strcpy(socket->alpn_storage[0], config->alpn);
    socket->alpn[0] = socket->alpn_storage[0];
    return H2_PAL_OK;
}

static h2_pal_result_t bk_net_tls_load_ca(
    bk_net_tls_socket_t *socket,
    const h2_pal_net_tls_config_t *config) {
    if (config->verify == H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY) {
        mbedtls_ssl_conf_authmode(&socket->config, MBEDTLS_SSL_VERIFY_NONE);
        return H2_PAL_OK;
    }
    mbedtls_ssl_conf_authmode(&socket->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    if (config->root_ca_pem == NULL || config->root_ca_pem_len == 0u) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    if (config->root_ca_pem_len == SIZE_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t *pem = h2_pal_mem_alloc(
        h2_bk_platform_default_allocator(), config->root_ca_pem_len + 1u);
    if (pem == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(pem, config->root_ca_pem, config->root_ca_pem_len);
    pem[config->root_ca_pem_len] = '\0';
    int result = mbedtls_x509_crt_parse(
        &socket->ca, pem, config->root_ca_pem_len + 1u);
    h2_pal_mem_free(h2_bk_platform_default_allocator(), pem);
    if (result != 0) {
        return H2_PAL_ERR_FORMAT;
    }
    mbedtls_ssl_conf_ca_chain(&socket->config, &socket->ca, NULL);
    return H2_PAL_OK;
}

static h2_pal_result_t bk_net_tls_handshake(
    bk_net_tls_socket_t *socket, uint32_t timeout_ms) {
    uint64_t deadline = bk_net_now_ms() + timeout_ms;
    for (;;) {
        int result = mbedtls_ssl_handshake(&socket->ssl);
        if (result == 0) {
            return mbedtls_ssl_get_verify_result(&socket->ssl) == 0u
                ? H2_PAL_OK
                : H2_PAL_ERR_TLS_VERIFY;
        }
        if (result != MBEDTLS_ERR_SSL_WANT_READ &&
            result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return result == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
                ? H2_PAL_ERR_TLS_VERIFY
                : H2_PAL_ERR_IO;
        }
        uint32_t remaining = bk_net_timeout_remaining_ms(deadline);
        if (remaining == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        int wait_result = bk_net_tls_wait(socket, result, remaining);
        if (wait_result != H2_PAL_OK && wait_result != H2_PAL_ERR_WOULD_BLOCK) {
            return wait_result;
        }
    }
}

static int family_to_lwip(h2_pal_net_family_t family) {
    return family == H2_PAL_NET_FAMILY_IPV6 ? -1 : AF_INET;
}

static int addr_to_sockaddr(
    const h2_pal_net_addr_t *addr,
    struct sockaddr_storage *storage,
    socklen_t *out_len) {
    if (addr == NULL || storage == NULL || out_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(storage, 0, sizeof(*storage));
    if (addr->family == H2_PAL_NET_FAMILY_IPV6) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    struct sockaddr_in *sin = (struct sockaddr_in *)storage;
    sin->sin_family = AF_INET;
    sin->sin_port = htons(addr->port);
    memcpy(&sin->sin_addr, addr->ip, 4u);
    *out_len = sizeof(*sin);
    return H2_PAL_OK;
}

static int sockaddr_to_addr(const struct sockaddr *sockaddr, h2_pal_net_addr_t *out_addr) {
    if (sockaddr == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_addr, 0, sizeof(*out_addr));
    if (sockaddr->sa_family != AF_INET) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sockaddr;
    out_addr->family = H2_PAL_NET_FAMILY_IPV4;
    out_addr->port = ntohs(sin->sin_port);
    memcpy(out_addr->ip, &sin->sin_addr, 4u);
    return H2_PAL_OK;
}

static void set_recv_timeout(int fd, uint32_t timeout_ms) {
    struct timeval timeout;
    timeout.tv_sec = (long)(timeout_ms / 1000u);
    timeout.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

static int wait_fd(int fd, int write_ready, uint32_t timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    int rc = select(fd + 1, write_ready ? NULL : &fds, write_ready ? &fds : NULL, NULL, &tv);
    if (rc == 0) {
        return H2_PAL_ERR_TIMEOUT;
    }
    if (rc < 0) {
        return errno == EINTR ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static int bk_net_socket_error(void) {
    return errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN
        ? H2_PAL_ERR_CLOSED
        : H2_PAL_ERR_IO;
}

static uint64_t bk_net_now_ms(void) {
    struct timeval now;
    return gettimeofday(&now, NULL) == 0
        ? (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_usec / 1000u
        : 0u;
}

static uint32_t bk_net_timeout_remaining_ms(uint64_t deadline_ms) {
    uint64_t now_ms = bk_net_now_ms();
    return now_ms >= deadline_ms
        ? 0u
        : (uint32_t)(deadline_ms - now_ms);
}

static int bk_net_resolve_host(
    const char *host,
    h2_pal_net_addr_t *out_addr) {
    if (host == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    int out_rc = sockaddr_to_addr(res->ai_addr, out_addr);
    freeaddrinfo(res);
    return out_rc;
}

static int bk_net_resolve_addr(
    void *user,
    const char *host,
    h2_pal_net_addr_t *out_addr) {
    (void)user;
    return bk_net_resolve_host(host, out_addr);
}

static h2_pal_result_t bk_net_resolver_reserve(void) {
    if (!s_bk_tls_mutex_ready ||
        rtos_lock_mutex(&s_bk_tls_mutex) != kNoErr) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    h2_pal_result_t result = s_bk_resolver_count < H2_BK_NET_RESOLVER_MAX
        ? H2_PAL_OK
        : H2_PAL_ERR_NO_SPACE;
    if (result == H2_PAL_OK) {
        s_bk_resolver_count += 1u;
    }
    (void)rtos_unlock_mutex(&s_bk_tls_mutex);
    return result;
}

static void bk_net_resolver_release(void) {
    if (!s_bk_tls_mutex_ready ||
        rtos_lock_mutex(&s_bk_tls_mutex) != kNoErr) {
        return;
    }
    if (s_bk_resolver_count > 0u) {
        s_bk_resolver_count -= 1u;
    }
    (void)rtos_unlock_mutex(&s_bk_tls_mutex);
}

static void bk_net_resolver_destroy(bk_net_resolver_t *resolver) {
    if (resolver == NULL) {
        return;
    }
    if (resolver->done != NULL) {
        (void)rtos_deinit_semaphore(&resolver->done);
    }
    if (resolver->lock != NULL) {
        (void)rtos_deinit_mutex(&resolver->lock);
    }
    h2_pal_mem_free(h2_bk_platform_default_allocator(), resolver);
    bk_net_resolver_release();
}

static void bk_net_resolver_worker(void *raw) {
    bk_net_resolver_t *resolver = (bk_net_resolver_t *)raw;
    h2_pal_net_addr_t addr;
    h2_pal_result_t result = bk_net_resolve_host(resolver->host, &addr);
    (void)rtos_lock_mutex(&resolver->lock);
    resolver->result = result;
    if (result == H2_PAL_OK) {
        resolver->addr = addr;
    }
    resolver->completed = 1;
    int closed = resolver->closed;
    if (!closed) {
        (void)rtos_set_semaphore(&resolver->done);
    }
    (void)rtos_unlock_mutex(&resolver->lock);
    if (closed) {
        bk_net_resolver_destroy(resolver);
    }
    rtos_delete_thread(NULL);
}

static h2_pal_result_t bk_net_resolve_start(
    void *user,
    const char *host,
    h2_pal_net_resolver_t **out_resolver) {
    (void)user;
    if (host == NULL || host[0] == '\0' || out_resolver == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_resolver = NULL;
    h2_pal_result_t reserve_result = bk_net_resolver_reserve();
    if (reserve_result != H2_PAL_OK) {
        return reserve_result;
    }
    size_t host_len = strlen(host);
    if (host_len >= SIZE_MAX - sizeof(bk_net_resolver_t)) {
        bk_net_resolver_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    bk_net_resolver_t *resolver = (bk_net_resolver_t *)h2_pal_mem_alloc(
        h2_bk_platform_default_allocator(),
        sizeof(*resolver) + host_len + 1u);
    if (resolver == NULL) {
        bk_net_resolver_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(resolver, 0, sizeof(*resolver));
    memcpy(resolver->host, host, host_len + 1u);
    if (rtos_init_mutex(&resolver->lock) != kNoErr) {
        bk_net_resolver_destroy(resolver);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (rtos_init_semaphore(&resolver->done, 1) != kNoErr) {
        bk_net_resolver_destroy(resolver);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (rtos_create_thread(
            NULL, BEKEN_APPLICATION_PRIORITY, "h2_dns",
            bk_net_resolver_worker, 4096u, resolver) != kNoErr) {
        bk_net_resolver_destroy(resolver);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *out_resolver = (h2_pal_net_resolver_t *)resolver;
    return H2_PAL_OK;
}

static h2_pal_result_t bk_net_resolve_poll(
    void *user,
    h2_pal_net_resolver_t *resolver_handle,
    h2_pal_net_addr_t *out_addr,
    uint32_t timeout_ms) {
    (void)user;
    bk_net_resolver_t *resolver = (bk_net_resolver_t *)resolver_handle;
    if (resolver == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)rtos_lock_mutex(&resolver->lock);
    if (resolver->closed) {
        (void)rtos_unlock_mutex(&resolver->lock);
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (resolver->completed) {
        h2_pal_result_t result = resolver->result;
        if (result == H2_PAL_OK) {
            *out_addr = resolver->addr;
        }
        (void)rtos_unlock_mutex(&resolver->lock);
        return result;
    }
    (void)rtos_unlock_mutex(&resolver->lock);

    bk_err_t wait_result =
        rtos_get_semaphore(&resolver->done, timeout_ms);
    (void)rtos_lock_mutex(&resolver->lock);
    if (!resolver->completed) {
        (void)rtos_unlock_mutex(&resolver->lock);
        return timeout_ms == 0u || wait_result == kNoErr
            ? H2_PAL_ERR_WOULD_BLOCK
            : H2_PAL_ERR_TIMEOUT;
    }
    h2_pal_result_t result = resolver->result;
    if (result == H2_PAL_OK) {
        *out_addr = resolver->addr;
    }
    (void)rtos_unlock_mutex(&resolver->lock);
    return result;
}

static void bk_net_resolve_close(
    void *user,
    h2_pal_net_resolver_t *resolver_handle) {
    (void)user;
    bk_net_resolver_t *resolver = (bk_net_resolver_t *)resolver_handle;
    if (resolver == NULL) {
        return;
    }
    (void)rtos_lock_mutex(&resolver->lock);
    resolver->closed = 1;
    int completed = resolver->completed;
    (void)rtos_unlock_mutex(&resolver->lock);
    if (completed) {
        bk_net_resolver_destroy(resolver);
    }
}

static int bk_net_get_host_addr(void *user, const char *iface_prefix, h2_pal_net_addr_t *out_addr) {
    (void)user;
    (void)iface_prefix;
    if (out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    netif_ip4_config_t sta_ip;
    memset(&sta_ip, 0, sizeof(sta_ip));
    if (bk_netif_get_ip4_config(NETIF_IF_STA, &sta_ip) == BK_OK && strcmp(sta_ip.ip, "0.0.0.0") != 0) {
        struct in_addr parsed;
        if (inet_aton(sta_ip.ip, &parsed) != 0) {
            out_addr->family = H2_PAL_NET_FAMILY_IPV4;
            memcpy(out_addr->ip, &parsed, 4u);
            return H2_PAL_OK;
        }
    }

    return H2_PAL_ERR_UNAVAILABLE;
}

static int bk_net_udp_open(
    void *user,
    h2_pal_net_family_t family,
    uint16_t port,
    h2_pal_net_socket_t *out_socket,
    h2_pal_net_addr_t *out_bind_addr) {
    (void)user;
    if (out_socket == NULL || out_bind_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_socket = -1;
    int lwip_family = family_to_lwip(family);
    if (lwip_family < 0) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    int fd = socket(lwip_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    set_recv_timeout(fd, 20u);

    h2_pal_net_addr_t bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.family = family;
    bind_addr.port = port;
    struct sockaddr_storage storage;
    socklen_t sock_len = 0;
    int rc = addr_to_sockaddr(&bind_addr, &storage, &sock_len);
    if (rc != H2_PAL_OK || bind(fd, (struct sockaddr *)&storage, sock_len) < 0) {
        closesocket(fd);
        return H2_PAL_ERR_IO;
    }
    if (getsockname(fd, (struct sockaddr *)&storage, &sock_len) < 0) {
        closesocket(fd);
        return H2_PAL_ERR_IO;
    }
    (void)sockaddr_to_addr((const struct sockaddr *)&storage, out_bind_addr);
    *out_socket = fd;
    return H2_PAL_OK;
}

static int bk_net_udp_sendto(
    void *user,
    h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *addr,
    const uint8_t *data,
    size_t len) {
    (void)user;
    if (socket_fd < 0 || addr == NULL || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct sockaddr_storage storage;
    socklen_t sock_len = 0;
    int rc = addr_to_sockaddr(addr, &storage, &sock_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    int sent = sendto(socket_fd, data, (int)len, 0, (struct sockaddr *)&storage, sock_len);
    return sent < 0 ? H2_PAL_ERR_IO : sent;
}

static int bk_net_udp_recvfrom(
    void *user,
    h2_pal_net_socket_t socket_fd,
    h2_pal_net_addr_t *out_addr,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    (void)user;
    if (socket_fd < 0 || data == NULL || len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    set_recv_timeout(socket_fd, timeout_ms);
    struct sockaddr_storage storage;
    socklen_t sock_len = sizeof(storage);
    int got = recvfrom(socket_fd, data, (int)len, 0, (struct sockaddr *)&storage, &sock_len);
    if (got < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
    }
    if (out_addr != NULL) {
        (void)sockaddr_to_addr((const struct sockaddr *)&storage, out_addr);
    }
    return got;
}

static int bk_net_udp_join_multicast(void *user, h2_pal_net_socket_t socket_fd, const h2_pal_net_addr_t *addr) {
    (void)user;
    if (socket_fd < 0 || addr == NULL || addr->family != H2_PAL_NET_FAMILY_IPV4) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    struct ip_mreq imreq;
    struct in_addr iaddr;
    memset(&imreq, 0, sizeof(imreq));
    memset(&iaddr, 0, sizeof(iaddr));
    memcpy(&imreq.imr_multiaddr, addr->ip, 4u);
    imreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF, &iaddr, sizeof(iaddr)) < 0) {
        return H2_PAL_ERR_IO;
    }
    return setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(imreq)) == 0
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static int bk_net_tcp_open(void *user, h2_pal_net_family_t family, h2_pal_net_socket_t *out_socket) {
    (void)user;
    if (out_socket == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int lwip_family = family_to_lwip(family);
    if (lwip_family < 0) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    int fd = socket(lwip_family, SOCK_STREAM, 0);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    *out_socket = fd;
    return H2_PAL_OK;
}

static int bk_net_tcp_open_bound(
    void *user,
    h2_pal_net_family_t family,
    const h2_pal_net_bind_t *bind_config,
    h2_pal_net_socket_t *out_socket) {
    if (bind_config == NULL || bind_config->type == H2_PAL_NET_BIND_DEFAULT) {
        return bk_net_tcp_open(user, family, out_socket);
    }
    if (bind_config->type != H2_PAL_NET_BIND_SOURCE_ADDR ||
        bind_config->source_addr.family != family) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    int result = bk_net_tcp_open(user, family, out_socket);
    if (result != H2_PAL_OK) {
        return result;
    }
    h2_pal_net_addr_t bind_addr = bind_config->source_addr;
    bind_addr.port = 0u;
    struct sockaddr_storage storage;
    socklen_t sock_len = 0;
    result = addr_to_sockaddr(&bind_addr, &storage, &sock_len);
    if (result != H2_PAL_OK ||
        bind(*out_socket, (struct sockaddr *)&storage, sock_len) < 0) {
        closesocket(*out_socket);
        *out_socket = -1;
        return result == H2_PAL_OK ? H2_PAL_ERR_IO : result;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t bk_net_tcp_connect(
    void *user,
    h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *addr,
    uint32_t timeout_ms) {
    (void)user;
    if (socket_fd < 0 || addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct sockaddr_storage storage;
    socklen_t sock_len = 0;
    int rc = addr_to_sockaddr(addr, &storage, &sock_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return H2_PAL_ERR_IO;
    }
    if (connect(socket_fd, (struct sockaddr *)&storage, sock_len) == 0 ||
        errno == EISCONN) {
        (void)fcntl(socket_fd, F_SETFL, flags & ~O_NONBLOCK);
        return H2_PAL_OK;
    }
    if (errno != EINPROGRESS && errno != EALREADY && errno != EINTR) {
        (void)fcntl(socket_fd, F_SETFL, flags);
        return H2_PAL_ERR_IO;
    }
    rc = wait_fd(socket_fd, 1, timeout_ms);
    if (rc != H2_PAL_OK) {
        if (rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK) {
            (void)fcntl(socket_fd, F_SETFL, flags);
        }
        return rc;
    }
    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) < 0 || so_error != 0) {
        (void)fcntl(socket_fd, F_SETFL, flags);
        return H2_PAL_ERR_IO;
    }
    return fcntl(socket_fd, F_SETFL, flags & ~O_NONBLOCK) == 0
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static int bk_net_tcp_listen(
    void *user,
    h2_pal_net_family_t family,
    uint16_t port,
    const h2_pal_net_bind_t *bind_config,
    h2_pal_net_socket_t *out_socket,
    h2_pal_net_addr_t *out_bind_addr) {
    (void)user;
    if (out_socket == NULL || out_bind_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_socket = -1;
    h2_pal_net_addr_t bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.family = family;
    if (bind_config != NULL && bind_config->type != H2_PAL_NET_BIND_DEFAULT) {
        if (bind_config->type != H2_PAL_NET_BIND_SOURCE_ADDR) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
        if (bind_config->source_addr.family != family) {
            return H2_PAL_ERR_INVALID_ARG;
        }
        bind_addr = bind_config->source_addr;
    }
    bind_addr.port = port;
    int lwip_family = family_to_lwip(family);
    if (lwip_family < 0) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    int fd = socket(lwip_family, SOCK_STREAM, 0);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_storage storage;
    socklen_t sock_len = 0;
    int rc = addr_to_sockaddr(&bind_addr, &storage, &sock_len);
    if (rc != H2_PAL_OK) {
        close(fd);
        return rc;
    }
    if (bind(fd, (struct sockaddr *)&storage, sock_len) < 0 ||
        listen(fd, 4) < 0) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    if (getsockname(fd, (struct sockaddr *)&storage, &sock_len) < 0) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    (void)sockaddr_to_addr((const struct sockaddr *)&storage, out_bind_addr);
    *out_socket = fd;
    return H2_PAL_OK;
}

static h2_pal_result_t bk_net_tcp_accept(
    void *user,
    h2_pal_net_socket_t listen_fd,
    h2_pal_net_socket_t *out_socket,
    h2_pal_net_addr_t *out_peer_addr,
    uint32_t timeout_ms) {
    (void)user;
    if (listen_fd < 0 || out_socket == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_socket = -1;
    int ready = wait_fd(listen_fd, 0, timeout_ms);
    if (ready != H2_PAL_OK) {
        return (h2_pal_result_t)ready;
    }
    struct sockaddr_storage storage;
    socklen_t sock_len = sizeof(storage);
    int fd = accept(listen_fd, (struct sockaddr *)&storage, &sock_len);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return H2_PAL_ERR_WOULD_BLOCK;
        }
        return H2_PAL_ERR_IO;
    }
    if (out_peer_addr != NULL) {
        (void)sockaddr_to_addr((const struct sockaddr *)&storage, out_peer_addr);
    }
    *out_socket = fd;
    return H2_PAL_OK;
}

static int bk_net_tcp_send(void *user, h2_pal_net_socket_t socket_fd, const uint8_t *data, size_t len) {
    (void)user;
    if (socket_fd < 0 || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    bk_net_tls_socket_t *tls_socket = NULL;
    size_t tls_slot = 0u;
    int tls_result = bk_net_tls_acquire(
        socket_fd, 0, 0u, 0u, &tls_socket, &tls_slot);
    if (tls_result < 0) {
        return tls_result;
    }
    if (tls_result > 0) {
        int result = mbedtls_ssl_write(&tls_socket->ssl, data, len);
        bk_net_tls_release(tls_slot);
        if (result > 0) {
            return result;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ ||
            result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return H2_PAL_ERR_WOULD_BLOCK;
        }
        return result == 0 ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_IO;
    }
    int sent = send(socket_fd, data, (int)len, 0);
    if (sent == 0) {
        return H2_PAL_ERR_CLOSED;
    }
    return sent < 0 ? bk_net_socket_error() : sent;
}

static int bk_net_tcp_send_timeout(
    void *user,
    h2_pal_net_socket_t socket_fd,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    (void)user;
    if (socket_fd < 0 || (data == NULL && len != 0u) ||
        len > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return 0;
    }
    uint64_t deadline_ms = bk_net_now_ms() + timeout_ms;
    bk_net_tls_socket_t *tls_socket = NULL;
    size_t tls_slot = 0u;
    int tls_result = bk_net_tls_acquire(
        socket_fd, 1, timeout_ms, deadline_ms, &tls_socket, &tls_slot);
    if (tls_result < 0) {
        return tls_result;
    }
    if (tls_result > 0) {
        for (;;) {
            int result = mbedtls_ssl_write(&tls_socket->ssl, data, len);
            if (result > 0) {
                bk_net_tls_release(tls_slot);
                return result;
            }
            if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                result != MBEDTLS_ERR_SSL_WANT_WRITE) {
                bk_net_tls_release(tls_slot);
                return result == 0 ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_IO;
            }
            uint32_t remaining = timeout_ms == 0u
                ? 0u
                : bk_net_timeout_remaining_ms(deadline_ms);
            int wait_result = bk_net_tls_wait(tls_socket, result, remaining);
            if (wait_result == H2_PAL_ERR_TIMEOUT) {
                wait_result = timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                               : H2_PAL_ERR_TIMEOUT;
            }
            if (wait_result != H2_PAL_OK &&
                wait_result != H2_PAL_ERR_WOULD_BLOCK) {
                bk_net_tls_release(tls_slot);
                return wait_result;
            }
            if (wait_result == H2_PAL_ERR_WOULD_BLOCK && timeout_ms == 0u) {
                bk_net_tls_release(tls_slot);
                return H2_PAL_ERR_WOULD_BLOCK;
            }
        }
    }
    for (;;) {
        uint32_t remaining_ms = timeout_ms == 0u
            ? 0u
            : bk_net_timeout_remaining_ms(deadline_ms);
        int ready = wait_fd(socket_fd, 1, remaining_ms);
        if (ready != H2_PAL_OK) {
            if (ready == H2_PAL_ERR_TIMEOUT) {
                return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                        : H2_PAL_ERR_TIMEOUT;
            }
            if (ready == H2_PAL_ERR_WOULD_BLOCK && timeout_ms != 0u &&
                remaining_ms != 0u) {
                continue;
            }
            return ready;
        }
        int sent = send(socket_fd, data, (int)len, MSG_DONTWAIT);
        if (sent > 0) {
            return sent;
        }
        if (sent == 0) {
            return H2_PAL_ERR_CLOSED;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return bk_net_socket_error();
        }
        if (timeout_ms == 0u) {
            return H2_PAL_ERR_WOULD_BLOCK;
        }
        if (bk_net_timeout_remaining_ms(deadline_ms) == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
    }
}

static int bk_net_tcp_recv(
    void *user,
    h2_pal_net_socket_t socket_fd,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    (void)user;
    if (socket_fd < 0 || data == NULL || len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint64_t deadline_ms = bk_net_now_ms() + timeout_ms;
    bk_net_tls_socket_t *tls_socket = NULL;
    size_t tls_slot = 0u;
    int tls_result = bk_net_tls_acquire(
        socket_fd, 1, timeout_ms, deadline_ms, &tls_socket, &tls_slot);
    if (tls_result < 0) {
        return tls_result;
    }
    if (tls_result > 0) {
        for (;;) {
            int result = mbedtls_ssl_read(&tls_socket->ssl, data, len);
            if (result > 0) {
                bk_net_tls_release(tls_slot);
                return result;
            }
            if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                bk_net_tls_release(tls_slot);
                return H2_PAL_ERR_CLOSED;
            }
            if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                result != MBEDTLS_ERR_SSL_WANT_WRITE) {
                bk_net_tls_release(tls_slot);
                return H2_PAL_ERR_IO;
            }
            uint32_t remaining = timeout_ms == 0u
                ? 0u
                : bk_net_timeout_remaining_ms(deadline_ms);
            int wait_result = bk_net_tls_wait(tls_socket, result, remaining);
            if (wait_result == H2_PAL_ERR_TIMEOUT) {
                wait_result = timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                               : H2_PAL_ERR_TIMEOUT;
            }
            if (wait_result != H2_PAL_OK &&
                wait_result != H2_PAL_ERR_WOULD_BLOCK) {
                bk_net_tls_release(tls_slot);
                return wait_result;
            }
            if (wait_result == H2_PAL_ERR_WOULD_BLOCK && timeout_ms == 0u) {
                bk_net_tls_release(tls_slot);
                return H2_PAL_ERR_WOULD_BLOCK;
            }
        }
    }
    set_recv_timeout(socket_fd, timeout_ms);
    int got = recv(socket_fd, data, (int)len, 0);
    if (got < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
    }
    return got == 0 ? H2_PAL_ERR_CLOSED : got;
}

static void bk_net_close(void *user, h2_pal_net_socket_t socket_fd) {
    (void)user;
    if (socket_fd >= 0) {
        if (s_bk_tls_mutex_ready &&
            rtos_lock_mutex(&s_bk_tls_mutex) == kNoErr) {
            int slot_index = bk_net_tls_find(socket_fd);
            if (slot_index >= 0) {
                bk_net_tls_socket_t *slot = &s_bk_tls_sockets[slot_index];
                uint32_t generation = s_bk_tls_generations[slot_index];
                slot->state = BK_NET_TLS_SOCKET_CLOSING;
                (void)rtos_unlock_mutex(&s_bk_tls_mutex);
                if (rtos_lock_mutex(&s_bk_tls_io_mutexes[slot_index]) ==
                    kNoErr) {
                    if (rtos_lock_mutex(&s_bk_tls_mutex) == kNoErr) {
                        if (slot->state == BK_NET_TLS_SOCKET_CLOSING &&
                            slot->fd == socket_fd &&
                            s_bk_tls_generations[slot_index] == generation) {
                            bk_net_tls_free(slot);
                            s_bk_tls_generations[slot_index] += 1u;
                        }
                        (void)rtos_unlock_mutex(&s_bk_tls_mutex);
                    }
                    bk_net_tls_release((size_t)slot_index);
                }
            } else {
                (void)rtos_unlock_mutex(&s_bk_tls_mutex);
            }
        }
        closesocket(socket_fd);
    }
}

static h2_pal_result_t bk_net_tls_wrap(
    void *user,
    h2_pal_net_socket_t socket_fd,
    const h2_pal_net_tls_config_t *config,
    uint32_t timeout_ms,
    h2_pal_net_socket_t *out_socket) {
    (void)user;
    if (socket_fd < 0 || config == NULL || out_socket == NULL ||
        config->server_name == NULL || config->server_name[0] == '\0' ||
        timeout_ms == 0u ||
        (config->root_ca_pem == NULL && config->root_ca_pem_len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_socket = -1;
    bk_net_tls_init();
    uint64_t deadline_ms = bk_net_now_ms() + timeout_ms;
    int lock_result = bk_net_tls_take_mutex(
        &s_bk_tls_mutex, 1, timeout_ms, deadline_ms);
    if (lock_result != H2_PAL_OK) {
        return lock_result;
    }
    if (bk_net_tls_find(socket_fd) >= 0) {
        (void)rtos_unlock_mutex(&s_bk_tls_mutex);
        return H2_PAL_ERR_INVALID_STATE;
    }
    int slot_index = bk_net_tls_find_free();
    if (slot_index < 0) {
        (void)rtos_unlock_mutex(&s_bk_tls_mutex);
        return H2_PAL_ERR_NO_MEMORY;
    }
    bk_net_tls_socket_t *slot = &s_bk_tls_sockets[slot_index];
    if (rtos_trylock_mutex(&s_bk_tls_io_mutexes[slot_index]) != kNoErr) {
        (void)rtos_unlock_mutex(&s_bk_tls_mutex);
        return H2_PAL_ERR_UNAVAILABLE;
    }
    memset(slot, 0, sizeof(*slot));
    slot->state = BK_NET_TLS_SOCKET_CONFIGURING;
    slot->fd = socket_fd;
    s_bk_tls_generations[slot_index] += 1u;
    uint32_t generation = s_bk_tls_generations[slot_index];
    (void)rtos_unlock_mutex(&s_bk_tls_mutex);
    mbedtls_ssl_init(&slot->ssl);
    mbedtls_ssl_config_init(&slot->config);
    mbedtls_x509_crt_init(&slot->ca);
    h2_pal_result_t rc = bk_net_tls_copy_alpn(slot, config);
    int result = rc == H2_PAL_OK
        ? mbedtls_ssl_config_defaults(
              &slot->config, MBEDTLS_SSL_IS_CLIENT,
              MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)
        : -1;
    if (result == 0) {
        mbedtls_ssl_conf_rng(&slot->config, bk_net_tls_random, slot);
        rc = bk_net_tls_load_ca(slot, config);
    } else if (rc == H2_PAL_OK) {
        rc = H2_PAL_ERR_IO;
    }
    if (rc == H2_PAL_OK && config->alpn != NULL && config->alpn[0] != '\0' &&
        mbedtls_ssl_conf_alpn_protocols(&slot->config, slot->alpn) != 0) {
        rc = H2_PAL_ERR_IO;
    }
    if (rc == H2_PAL_OK && mbedtls_ssl_setup(&slot->ssl, &slot->config) != 0) {
        rc = H2_PAL_ERR_IO;
    }
    if (rc == H2_PAL_OK &&
        mbedtls_ssl_set_hostname(&slot->ssl, config->server_name) != 0) {
        rc = H2_PAL_ERR_INVALID_ARG;
    }
    if (rc == H2_PAL_OK) {
        mbedtls_ssl_set_bio(
            &slot->ssl, slot, bk_net_tls_send_raw, bk_net_tls_recv_raw, NULL);
        uint32_t remaining_ms = bk_net_timeout_remaining_ms(deadline_ms);
        rc = remaining_ms == 0u
            ? H2_PAL_ERR_TIMEOUT
            : bk_net_tls_handshake(slot, remaining_ms);
    }
    if (rtos_lock_mutex(&s_bk_tls_mutex) == kNoErr) {
        if (slot->state == BK_NET_TLS_SOCKET_CONFIGURING &&
            slot->fd == socket_fd &&
            s_bk_tls_generations[slot_index] == generation) {
            if (rc != H2_PAL_OK) {
                bk_net_tls_free(slot);
                s_bk_tls_generations[slot_index] += 1u;
            } else {
                slot->state = BK_NET_TLS_SOCKET_ACTIVE;
                *out_socket = socket_fd;
            }
        } else {
            rc = H2_PAL_ERR_CLOSED;
        }
        (void)rtos_unlock_mutex(&s_bk_tls_mutex);
    } else {
        rc = H2_PAL_ERR_UNAVAILABLE;
    }
    bk_net_tls_release((size_t)slot_index);
    return rc;
}

const h2_pal_net_api_t *h2_bk_platform_net_api(void) {
    bk_net_tls_init();
    static const h2_pal_net_vtable_t vtable = {
        .resolve_addr = bk_net_resolve_addr,
        .resolve_start = bk_net_resolve_start,
        .resolve_poll = bk_net_resolve_poll,
        .resolve_close = bk_net_resolve_close,
        .get_host_addr = bk_net_get_host_addr,
        .udp_open = bk_net_udp_open,
        .udp_sendto = bk_net_udp_sendto,
        .udp_recvfrom = bk_net_udp_recvfrom,
        .udp_join_multicast = bk_net_udp_join_multicast,
        .tcp_open = bk_net_tcp_open,
        .tcp_open_bound = bk_net_tcp_open_bound,
        .tcp_connect = bk_net_tcp_connect,
        .tcp_send = bk_net_tcp_send,
        .tcp_send_timeout = bk_net_tcp_send_timeout,
        .tcp_recv = bk_net_tcp_recv,
        .tls_wrap = bk_net_tls_wrap,
        .close = bk_net_close,
        .tcp_listen = bk_net_tcp_listen,
        .tcp_accept = bk_net_tcp_accept,
    };
    static const h2_pal_net_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
