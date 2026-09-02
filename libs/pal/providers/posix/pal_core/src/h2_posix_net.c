#include "h2_posix_pal_core.h"

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

#define H2_DESKTOP_TLS_SOCKET_MAX 32u
#define H2_DESKTOP_NET_RESOLVER_MAX 8u

typedef struct desktop_tls_socket {
    int in_use;
    int closing;
    uint32_t generation;
    size_t refs;
    h2_pal_net_socket_t fd;
    WOLFSSL *ssl;
    WOLFSSL_CTX *ctx;
    pthread_mutex_t lock;
} desktop_tls_socket_t;

typedef struct desktop_sigpipe_guard {
    sigset_t mask;
    sigset_t previous_mask;
    int consume_generated_signal;
} desktop_sigpipe_guard_t;

typedef struct desktop_net_resolver {
    pthread_mutex_t lock;
    pthread_cond_t done;
    int completed;
    int closed;
    h2_pal_result_t result;
    h2_pal_net_addr_t addr;
    char host[];
} desktop_net_resolver_t;

static desktop_tls_socket_t s_tls_sockets[H2_DESKTOP_TLS_SOCKET_MAX];
static pthread_mutex_t s_tls_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_tls_generation;
static size_t s_resolver_count;

/* wolfSSL can write during handshake, read, and shutdown. Keep SIGPIPE local to
 * the operation without changing the process-wide signal disposition. */
static int sigpipe_guard_begin(desktop_sigpipe_guard_t *guard) {
    if (guard == NULL || sigemptyset(&guard->mask) != 0 || sigaddset(&guard->mask, SIGPIPE) != 0) {
        return 0;
    }
    if (pthread_sigmask(SIG_BLOCK, &guard->mask, &guard->previous_mask) != 0) {
        return 0;
    }
    sigset_t pending;
    int was_blocked = sigismember(&guard->previous_mask, SIGPIPE);
    int was_pending = sigpending(&pending) == 0 ? sigismember(&pending, SIGPIPE) : -1;
    if (was_blocked < 0 || was_pending < 0) {
        (void)pthread_sigmask(SIG_SETMASK, &guard->previous_mask, NULL);
        return 0;
    }
    guard->consume_generated_signal = !was_blocked && !was_pending;
    return 1;
}

static void sigpipe_guard_end(desktop_sigpipe_guard_t *guard) {
    if (guard == NULL) {
        return;
    }
    int operation_errno = errno;
    sigset_t pending;
    if (guard->consume_generated_signal && sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
        int signal_number = 0;
        (void)sigwait(&guard->mask, &signal_number);
    }
    (void)pthread_sigmask(SIG_SETMASK, &guard->previous_mask, NULL);
    errno = operation_errno;
}

static int family_to_posix(h2_pal_net_family_t family) {
    return family == H2_PAL_NET_FAMILY_IPV6 ? AF_INET6 : AF_INET;
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
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)storage;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(addr->port);
        memcpy(&sin6->sin6_addr, addr->ip, 16u);
        *out_len = sizeof(*sin6);
    } else {
        struct sockaddr_in *sin = (struct sockaddr_in *)storage;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(addr->port);
        memcpy(&sin->sin_addr, addr->ip, 4u);
        *out_len = sizeof(*sin);
    }
    return H2_PAL_OK;
}

static int sockaddr_to_addr(
    const struct sockaddr *sockaddr,
    h2_pal_net_addr_t *out_addr) {
    if (sockaddr == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_addr, 0, sizeof(*out_addr));
    if (sockaddr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sockaddr;
        out_addr->family = H2_PAL_NET_FAMILY_IPV6;
        out_addr->port = ntohs(sin6->sin6_port);
        memcpy(out_addr->ip, &sin6->sin6_addr, 16u);
    } else {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sockaddr;
        out_addr->family = H2_PAL_NET_FAMILY_IPV4;
        out_addr->port = ntohs(sin->sin_port);
        memcpy(out_addr->ip, &sin->sin_addr, 4u);
    }
    return H2_PAL_OK;
}

static int wait_fd(int fd, int write_ready, uint32_t timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec = (time_t)(timeout_ms / 1000u);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
    int rc = select(fd + 1, write_ready ? NULL : &fds, write_ready ? &fds : NULL, NULL, &tv);
    if (rc == 0) {
        return H2_PAL_ERR_TIMEOUT;
    }
    if (rc < 0) {
        return errno == EINTR ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

static int desktop_net_socket_error(void) {
    return errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN
        ? H2_PAL_ERR_CLOSED
        : H2_PAL_ERR_IO;
}

static uint64_t desktop_now_ms(void) {
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        return 0u;
    }
    return ((uint64_t)now.tv_sec * 1000u) + ((uint64_t)now.tv_usec / 1000u);
}

static uint64_t timeout_deadline_ms(uint32_t timeout_ms) {
    return desktop_now_ms() + (uint64_t)timeout_ms;
}

static uint32_t timeout_remaining_ms(uint64_t deadline_ms) {
    uint64_t now = desktop_now_ms();
    if (now >= deadline_ms) {
        return 0u;
    }
    uint64_t remaining = deadline_ms - now;
    return remaining > (uint64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static int tls_find_slot(h2_pal_net_socket_t socket_fd) {
    for (size_t i = 0; i < H2_DESKTOP_TLS_SOCKET_MAX; ++i) {
        if (s_tls_sockets[i].in_use && s_tls_sockets[i].fd == socket_fd) {
            return (int)i;
        }
    }
    return -1;
}

static int tls_find_free_slot(void) {
    for (size_t i = 0; i < H2_DESKTOP_TLS_SOCKET_MAX; ++i) {
        if (!s_tls_sockets[i].in_use) {
            return (int)i;
        }
    }
    return -1;
}

static void tls_free_handles(WOLFSSL *ssl, WOLFSSL_CTX *ctx) {
    if (ssl != NULL) {
        desktop_sigpipe_guard_t guard;
        if (sigpipe_guard_begin(&guard)) {
            (void)wolfSSL_shutdown(ssl);
            sigpipe_guard_end(&guard);
        }
        wolfSSL_free(ssl);
    }
    if (ctx != NULL) {
        wolfSSL_CTX_free(ctx);
    }
}

static void tls_detach_slot_locked(
    int slot,
    WOLFSSL **out_ssl,
    WOLFSSL_CTX **out_ctx) {
    if (slot < 0) {
        return;
    }
    if (out_ssl != NULL) {
        *out_ssl = s_tls_sockets[slot].ssl;
    }
    if (out_ctx != NULL) {
        *out_ctx = s_tls_sockets[slot].ctx;
    }
    s_tls_sockets[slot].in_use = 0;
    s_tls_sockets[slot].closing = 0;
    s_tls_sockets[slot].generation = 0u;
    s_tls_sockets[slot].refs = 0u;
    s_tls_sockets[slot].fd = -1;
    s_tls_sockets[slot].ssl = NULL;
    s_tls_sockets[slot].ctx = NULL;
    (void)pthread_mutex_destroy(&s_tls_sockets[slot].lock);
}

static void tls_clear_reserved_slot(int slot, uint32_t generation) {
    if (slot < 0 || !s_tls_sockets[slot].in_use || s_tls_sockets[slot].generation != generation) {
        return;
    }
    tls_detach_slot_locked(slot, NULL, NULL);
}

static int tls_acquire_slot(
    h2_pal_net_socket_t socket_fd,
    int *out_slot,
    WOLFSSL **out_ssl) {
    if (out_slot == NULL || out_ssl == NULL) {
        return 0;
    }
    *out_slot = -1;
    *out_ssl = NULL;
    (void)pthread_mutex_lock(&s_tls_lock);
    int slot = tls_find_slot(socket_fd);
    if (slot >= 0 && !s_tls_sockets[slot].closing && s_tls_sockets[slot].ssl != NULL) {
        s_tls_sockets[slot].refs += 1u;
        *out_slot = slot;
        *out_ssl = s_tls_sockets[slot].ssl;
        (void)pthread_mutex_unlock(&s_tls_lock);
        return 1;
    }
    (void)pthread_mutex_unlock(&s_tls_lock);
    return 0;
}

static void tls_release_slot(int slot) {
    WOLFSSL *ssl = NULL;
    WOLFSSL_CTX *ctx = NULL;
    (void)pthread_mutex_lock(&s_tls_lock);
    if (slot >= 0 && s_tls_sockets[slot].in_use && s_tls_sockets[slot].refs > 0u) {
        s_tls_sockets[slot].refs -= 1u;
        if (s_tls_sockets[slot].closing && s_tls_sockets[slot].refs == 0u) {
            tls_detach_slot_locked(slot, &ssl, &ctx);
        }
    }
    (void)pthread_mutex_unlock(&s_tls_lock);
    tls_free_handles(ssl, ctx);
}

static h2_pal_result_t tls_load_root_ca(
    WOLFSSL_CTX *ctx,
    const h2_pal_net_tls_config_t *config) {
    if (config->root_ca_pem == NULL || config->root_ca_pem_len == 0u) {
        return wolfSSL_CTX_load_system_CA_certs(ctx) == WOLFSSL_SUCCESS
                   ? H2_PAL_OK
                   : H2_PAL_ERR_IO;
    }
    if (config->root_ca_pem_len > (size_t)INT32_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return wolfSSL_CTX_load_verify_buffer(
               ctx,
               config->root_ca_pem,
               (long)config->root_ca_pem_len,
               WOLFSSL_FILETYPE_PEM) == WOLFSSL_SUCCESS
               ? H2_PAL_OK
               : H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t tls_configure_alpn(WOLFSSL *ssl, const char *alpn) {
    if (alpn == NULL || alpn[0] == '\0') {
        return H2_PAL_OK;
    }
    size_t len = strlen(alpn);
    if (len > 255u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return wolfSSL_UseALPN(
               ssl,
               (char *)alpn,
               (word32)len,
               WOLFSSL_ALPN_FAILED_ON_MISMATCH) == WOLFSSL_SUCCESS
               ? H2_PAL_OK
               : H2_PAL_ERR_UNSUPPORTED;
}

static int tls_is_verify_error(int error) {
    return error == DOMAIN_NAME_MISMATCH || error == IPADDR_MISMATCH ||
           error == VERIFY_CERT_ERROR || error == VERIFY_SIGN_ERROR ||
           error == ASN_NO_SIGNER_E || error == ASN_SELF_SIGNED_E ||
           error == ASN_SIG_CONFIRM_E || error == ASN_BEFORE_DATE_E ||
           error == ASN_AFTER_DATE_E;
}

static h2_pal_result_t tls_wait_handshake(
    WOLFSSL *ssl,
    int fd,
    uint32_t timeout_ms,
    h2_pal_net_tls_verify_t verify_mode) {
    uint64_t deadline = timeout_deadline_ms(timeout_ms);
    for (;;) {
        desktop_sigpipe_guard_t guard;
        if (!sigpipe_guard_begin(&guard)) {
            return H2_PAL_ERR_IO;
        }
        int rc = wolfSSL_connect(ssl);
        sigpipe_guard_end(&guard);
        if (rc == 1) {
            return H2_PAL_OK;
        }
        int err = wolfSSL_get_error(ssl, rc);
        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
            uint32_t remaining = timeout_remaining_ms(deadline);
            if (remaining == 0u) {
                return H2_PAL_ERR_TIMEOUT;
            }
            h2_pal_result_t ready = wait_fd(
                fd, err == WOLFSSL_ERROR_WANT_WRITE, remaining);
            if (ready != H2_PAL_OK) {
                return ready;
            }
            continue;
        }
        if (verify_mode == H2_PAL_NET_TLS_VERIFY_REQUIRED &&
            tls_is_verify_error(err)) {
            return H2_PAL_ERR_TLS_VERIFY;
        }
        return H2_PAL_ERR_IO;
    }
}

static int desktop_net_resolve_host(
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

static int desktop_net_resolve_addr(
    void *user,
    const char *host,
    h2_pal_net_addr_t *out_addr) {
    (void)user;
    return desktop_net_resolve_host(host, out_addr);
}

static h2_pal_result_t desktop_net_resolver_reserve(void) {
    (void)pthread_mutex_lock(&s_tls_lock);
    h2_pal_result_t result = s_resolver_count < H2_DESKTOP_NET_RESOLVER_MAX
        ? H2_PAL_OK
        : H2_PAL_ERR_NO_SPACE;
    if (result == H2_PAL_OK) {
        s_resolver_count += 1u;
    }
    (void)pthread_mutex_unlock(&s_tls_lock);
    return result;
}

static void desktop_net_resolver_release(void) {
    (void)pthread_mutex_lock(&s_tls_lock);
    if (s_resolver_count > 0u) {
        s_resolver_count -= 1u;
    }
    (void)pthread_mutex_unlock(&s_tls_lock);
}

static void desktop_net_resolver_destroy(desktop_net_resolver_t *resolver) {
    if (resolver == NULL) {
        return;
    }
    (void)pthread_cond_destroy(&resolver->done);
    (void)pthread_mutex_destroy(&resolver->lock);
    free(resolver);
    desktop_net_resolver_release();
}

static void *desktop_net_resolver_worker(void *raw) {
    desktop_net_resolver_t *resolver = (desktop_net_resolver_t *)raw;
    h2_pal_net_addr_t addr;
    h2_pal_result_t result =
        desktop_net_resolve_host(resolver->host, &addr);
    (void)pthread_mutex_lock(&resolver->lock);
    resolver->result = result;
    if (result == H2_PAL_OK) {
        resolver->addr = addr;
    }
    resolver->completed = 1;
    int closed = resolver->closed;
    if (!closed) {
        (void)pthread_cond_broadcast(&resolver->done);
    }
    (void)pthread_mutex_unlock(&resolver->lock);
    if (closed) {
        desktop_net_resolver_destroy(resolver);
    }
    return NULL;
}

static h2_pal_result_t desktop_net_resolve_start(
    void *user,
    const char *host,
    h2_pal_net_resolver_t **out_resolver) {
    (void)user;
    if (host == NULL || host[0] == '\0' || out_resolver == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_resolver = NULL;
    h2_pal_result_t reserve_result = desktop_net_resolver_reserve();
    if (reserve_result != H2_PAL_OK) {
        return reserve_result;
    }
    size_t host_len = strlen(host);
    if (host_len >= SIZE_MAX - sizeof(desktop_net_resolver_t)) {
        desktop_net_resolver_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    desktop_net_resolver_t *resolver =
        (desktop_net_resolver_t *)calloc(
            1u, sizeof(*resolver) + host_len + 1u);
    if (resolver == NULL) {
        desktop_net_resolver_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(resolver->host, host, host_len + 1u);
    if (pthread_mutex_init(&resolver->lock, NULL) != 0) {
        free(resolver);
        desktop_net_resolver_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (pthread_cond_init(&resolver->done, NULL) != 0) {
        (void)pthread_mutex_destroy(&resolver->lock);
        free(resolver);
        desktop_net_resolver_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    pthread_attr_t attributes;
    int rc = pthread_attr_init(&attributes);
    int attributes_ready = rc == 0;
    if (rc == 0) {
        rc = pthread_attr_setdetachstate(
            &attributes, PTHREAD_CREATE_DETACHED);
    }
    pthread_t worker;
    if (rc == 0) {
        rc = pthread_create(
            &worker, &attributes, desktop_net_resolver_worker, resolver);
    }
    if (attributes_ready) {
        (void)pthread_attr_destroy(&attributes);
    }
    if (rc != 0) {
        desktop_net_resolver_destroy(resolver);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *out_resolver = (h2_pal_net_resolver_t *)resolver;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_net_resolve_poll(
    void *user,
    h2_pal_net_resolver_t *resolver_handle,
    h2_pal_net_addr_t *out_addr,
    uint32_t timeout_ms) {
    (void)user;
    desktop_net_resolver_t *resolver =
        (desktop_net_resolver_t *)resolver_handle;
    if (resolver == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&resolver->lock);
    if (resolver->closed) {
        (void)pthread_mutex_unlock(&resolver->lock);
        return H2_PAL_ERR_INVALID_STATE;
    }
    int wait_result = 0;
    if (!resolver->completed && timeout_ms != 0u) {
        struct timeval now;
        (void)gettimeofday(&now, NULL);
        uint64_t deadline_us = (uint64_t)now.tv_sec * 1000000u +
            (uint64_t)now.tv_usec + (uint64_t)timeout_ms * 1000u;
        struct timespec deadline = {
            .tv_sec = (time_t)(deadline_us / 1000000u),
            .tv_nsec = (long)((deadline_us % 1000000u) * 1000u),
        };
        while (!resolver->completed && wait_result == 0) {
            wait_result = pthread_cond_timedwait(
                &resolver->done, &resolver->lock, &deadline);
        }
    }
    if (!resolver->completed) {
        (void)pthread_mutex_unlock(&resolver->lock);
        return timeout_ms == 0u
            ? H2_PAL_ERR_WOULD_BLOCK
            : H2_PAL_ERR_TIMEOUT;
    }
    h2_pal_result_t result = resolver->result;
    if (result == H2_PAL_OK) {
        *out_addr = resolver->addr;
    }
    (void)pthread_mutex_unlock(&resolver->lock);
    return result;
}

static void desktop_net_resolve_close(
    void *user,
    h2_pal_net_resolver_t *resolver_handle) {
    (void)user;
    desktop_net_resolver_t *resolver =
        (desktop_net_resolver_t *)resolver_handle;
    if (resolver == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&resolver->lock);
    resolver->closed = 1;
    int completed = resolver->completed;
    (void)pthread_mutex_unlock(&resolver->lock);
    if (completed) {
        desktop_net_resolver_destroy(resolver);
    }
}

static int desktop_net_get_host_addr(void *user, const char *iface_prefix, h2_pal_net_addr_t *out_addr) {
    (void)user;
    if (out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == 0) {
        for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
                continue;
            }
            if (iface_prefix != NULL && iface_prefix[0] != '\0' &&
                strncmp(ifa->ifa_name, iface_prefix, strlen(iface_prefix)) != 0) {
                continue;
            }
            int rc = sockaddr_to_addr(ifa->ifa_addr, out_addr);
            freeifaddrs(ifaddr);
            return rc;
        }
        freeifaddrs(ifaddr);
    }
    out_addr->family = H2_PAL_NET_FAMILY_IPV4;
    out_addr->ip[0] = 127u;
    out_addr->ip[1] = 0u;
    out_addr->ip[2] = 0u;
    out_addr->ip[3] = 1u;
    return H2_PAL_OK;
}

/* Datagram sockets default to a small send buffer on some hosts (macOS caps
 * a datagram at SO_SNDBUF, 9216 bytes by default), which rejects otherwise
 * valid UDP payloads with EMSGSIZE. Raise both buffers best-effort so any
 * datagram up to the IP maximum can be sent and bursts are not dropped. */
static void desktop_net_udp_grow_buffers(int fd) {
    int size = 256 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

static int desktop_net_udp_open(
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
    int fd = socket(family_to_posix(family), SOCK_DGRAM, 0);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    desktop_net_udp_grow_buffers(fd);

    struct sockaddr_storage storage;
    socklen_t len = 0;
    h2_pal_net_addr_t bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.family = family;
    bind_addr.port = port;
    int rc = addr_to_sockaddr(&bind_addr, &storage, &len);
    if (rc != H2_PAL_OK || bind(fd, (struct sockaddr *)&storage, len) < 0) {
        close(fd);
        return rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc;
    }
    if (getsockname(fd, (struct sockaddr *)&storage, &len) < 0) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    (void)sockaddr_to_addr((const struct sockaddr *)&storage, out_bind_addr);
    *out_socket = fd;
    return H2_PAL_OK;
}

static int desktop_net_udp_open_bound(
    void *user,
    h2_pal_net_family_t family,
    uint16_t port,
    const h2_pal_net_bind_t *bind_config,
    h2_pal_net_socket_t *out_socket,
    h2_pal_net_addr_t *out_bind_addr) {
    if (bind_config == NULL || bind_config->type == H2_PAL_NET_BIND_DEFAULT) {
        return desktop_net_udp_open(user, family, port, out_socket, out_bind_addr);
    }
    if (bind_config->type != H2_PAL_NET_BIND_SOURCE_ADDR) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (out_socket == NULL || out_bind_addr == NULL || bind_config->source_addr.family != family) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_socket = -1;
    int fd = socket(family_to_posix(family), SOCK_DGRAM, 0);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    desktop_net_udp_grow_buffers(fd);

    h2_pal_net_addr_t bind_addr = bind_config->source_addr;
    bind_addr.port = port;
    struct sockaddr_storage storage;
    socklen_t len = 0;
    int rc = addr_to_sockaddr(&bind_addr, &storage, &len);
    if (rc != H2_PAL_OK || bind(fd, (struct sockaddr *)&storage, len) < 0) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    if (getsockname(fd, (struct sockaddr *)&storage, &len) < 0) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    (void)sockaddr_to_addr((const struct sockaddr *)&storage, out_bind_addr);
    *out_socket = fd;
    return H2_PAL_OK;
}

static int desktop_net_udp_sendto(
    void *user,
    h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *addr,
    const uint8_t *data,
    size_t len) {
    (void)user;
    if (addr == NULL || (data == NULL && len != 0u) || socket_fd < 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct sockaddr_storage storage;
    socklen_t sock_len = 0;
    int rc = addr_to_sockaddr(addr, &storage, &sock_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    ssize_t sent = sendto(socket_fd, data, len, 0, (struct sockaddr *)&storage, sock_len);
    return sent < 0 ? H2_PAL_ERR_IO : (int)sent;
}

static int desktop_net_udp_recvfrom(
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
    int ready = wait_fd(socket_fd, 0, timeout_ms);
    if (ready != H2_PAL_OK) {
        return ready;
    }
    struct sockaddr_storage storage;
    socklen_t sock_len = sizeof(storage);
    ssize_t got = recvfrom(socket_fd, data, len, 0, (struct sockaddr *)&storage, &sock_len);
    if (got < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_IO;
    }
    if (out_addr != NULL) {
        (void)sockaddr_to_addr((const struct sockaddr *)&storage, out_addr);
    }
    return (int)got;
}

static int desktop_net_udp_join_multicast(
    void *user,
    h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *addr) {
    (void)user;
    if (socket_fd < 0 || addr == NULL || addr->family != H2_PAL_NET_FAMILY_IPV4) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    struct ip_mreq imreq;
    memset(&imreq, 0, sizeof(imreq));
    memcpy(&imreq.imr_multiaddr, addr->ip, 4u);
    imreq.imr_interface.s_addr = INADDR_ANY;
    return setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(imreq)) == 0
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static int desktop_net_tcp_open(void *user, h2_pal_net_family_t family, h2_pal_net_socket_t *out_socket) {
    (void)user;
    if (out_socket == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int fd = socket(family_to_posix(family), SOCK_STREAM, 0);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    *out_socket = fd;
    return H2_PAL_OK;
}

static int desktop_net_tcp_open_bound(
    void *user,
    h2_pal_net_family_t family,
    const h2_pal_net_bind_t *bind_config,
    h2_pal_net_socket_t *out_socket) {
    if (bind_config == NULL || bind_config->type == H2_PAL_NET_BIND_DEFAULT) {
        return desktop_net_tcp_open(user, family, out_socket);
    }
    if (bind_config->type != H2_PAL_NET_BIND_SOURCE_ADDR) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    int rc = desktop_net_tcp_open(user, family, out_socket);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_pal_net_addr_t bind_addr = bind_config->source_addr;
    bind_addr.port = 0u;
    struct sockaddr_storage storage;
    socklen_t len = 0;
    rc = addr_to_sockaddr(&bind_addr, &storage, &len);
    if (rc != H2_PAL_OK || bind(*out_socket, (struct sockaddr *)&storage, len) < 0) {
        close(*out_socket);
        *out_socket = -1;
        return rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_net_tcp_connect(
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
    if (flags < 0) {
        return H2_PAL_ERR_IO;
    }
    if (fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
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

static int desktop_net_tcp_send(void *user, h2_pal_net_socket_t socket_fd, const uint8_t *data, size_t len) {
    (void)user;
    if (socket_fd < 0 || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int tls_slot = -1;
    WOLFSSL *ssl = NULL;
    if (tls_acquire_slot(socket_fd, &tls_slot, &ssl)) {
        (void)pthread_mutex_lock(&s_tls_sockets[tls_slot].lock);
        desktop_sigpipe_guard_t guard;
        if (!sigpipe_guard_begin(&guard)) {
            (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
            tls_release_slot(tls_slot);
            return H2_PAL_ERR_IO;
        }
        int sent = wolfSSL_write(ssl, data, (int)len);
        sigpipe_guard_end(&guard);
        if (sent > 0) {
            (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
            tls_release_slot(tls_slot);
            return sent;
        }
        int err = wolfSSL_get_error(ssl, sent);
        (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
        tls_release_slot(tls_slot);
        if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
            return H2_PAL_ERR_WOULD_BLOCK;
        }
        return err == WOLFSSL_ERROR_ZERO_RETURN ? H2_PAL_ERR_CLOSED
                                                : H2_PAL_ERR_IO;
    }
    desktop_sigpipe_guard_t guard;
    if (!sigpipe_guard_begin(&guard)) {
        return H2_PAL_ERR_IO;
    }
    ssize_t sent = send(socket_fd, data, len, 0);
    sigpipe_guard_end(&guard);
    if (sent == 0) {
        return H2_PAL_ERR_CLOSED;
    }
    return sent < 0 ? desktop_net_socket_error() : (int)sent;
}

static int desktop_net_tcp_send_timeout(
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
    int tls_slot = -1;
    WOLFSSL *ssl = NULL;
    if (tls_acquire_slot(socket_fd, &tls_slot, &ssl)) {
        (void)pthread_mutex_lock(&s_tls_sockets[tls_slot].lock);
        uint64_t deadline = timeout_deadline_ms(timeout_ms);
        for (;;) {
            desktop_sigpipe_guard_t guard;
            if (!sigpipe_guard_begin(&guard)) {
                (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
                tls_release_slot(tls_slot);
                return H2_PAL_ERR_IO;
            }
            int sent = wolfSSL_write(ssl, data, (int)len);
            sigpipe_guard_end(&guard);
            if (sent > 0) {
                (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
                tls_release_slot(tls_slot);
                return sent;
            }
            int err = wolfSSL_get_error(ssl, sent);
            if (err != WOLFSSL_ERROR_WANT_READ &&
                err != WOLFSSL_ERROR_WANT_WRITE) {
                (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
                tls_release_slot(tls_slot);
                return err == WOLFSSL_ERROR_ZERO_RETURN ? H2_PAL_ERR_CLOSED
                                                        : H2_PAL_ERR_IO;
            }
            uint32_t remaining = timeout_ms == 0u
                ? 0u
                : timeout_remaining_ms(deadline);
            int ready = wait_fd(
                socket_fd, err == WOLFSSL_ERROR_WANT_WRITE, remaining);
            if (ready == H2_PAL_ERR_TIMEOUT) {
                ready = timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                         : H2_PAL_ERR_TIMEOUT;
            } else if (ready == H2_PAL_ERR_WOULD_BLOCK && timeout_ms != 0u &&
                       remaining != 0u) {
                continue;
            }
            if (ready != H2_PAL_OK) {
                (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
                tls_release_slot(tls_slot);
                return ready;
            }
        }
    }
    uint64_t deadline = timeout_deadline_ms(timeout_ms);
    for (;;) {
        uint32_t remaining = timeout_ms == 0u
            ? 0u
            : timeout_remaining_ms(deadline);
        int ready = wait_fd(socket_fd, 1, remaining);
        if (ready != H2_PAL_OK) {
            if (ready == H2_PAL_ERR_TIMEOUT) {
                return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                        : H2_PAL_ERR_TIMEOUT;
            }
            if (ready == H2_PAL_ERR_WOULD_BLOCK && timeout_ms != 0u &&
                remaining != 0u) {
                continue;
            }
            return ready;
        }
        desktop_sigpipe_guard_t guard;
        if (!sigpipe_guard_begin(&guard)) {
            return H2_PAL_ERR_IO;
        }
        int flags = fcntl(socket_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            sigpipe_guard_end(&guard);
            return H2_PAL_ERR_IO;
        }
        ssize_t sent = send(socket_fd, data, len, 0);
        int send_errno = errno;
        (void)fcntl(socket_fd, F_SETFL, flags);
        errno = send_errno;
        sigpipe_guard_end(&guard);
        if (sent > 0) {
            return (int)sent;
        }
        if (sent == 0) {
            return H2_PAL_ERR_CLOSED;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (timeout_ms == 0u) {
                return H2_PAL_ERR_WOULD_BLOCK;
            }
            if (timeout_remaining_ms(deadline) == 0u) {
                return H2_PAL_ERR_TIMEOUT;
            }
            continue;
        }
        return desktop_net_socket_error();
    }
}

static int desktop_net_tcp_recv(
    void *user,
    h2_pal_net_socket_t socket_fd,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    (void)user;
    if (socket_fd < 0 || data == NULL || len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int tls_slot = -1;
    WOLFSSL *ssl = NULL;
    if (tls_acquire_slot(socket_fd, &tls_slot, &ssl)) {
        (void)pthread_mutex_lock(&s_tls_sockets[tls_slot].lock);
        uint64_t deadline = timeout_deadline_ms(timeout_ms);
        for (;;) {
            if (wolfSSL_pending(ssl) <= 0) {
                uint32_t remaining = timeout_remaining_ms(deadline);
                if (remaining == 0u) {
                    (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
                    tls_release_slot(tls_slot);
                    return H2_PAL_ERR_TIMEOUT;
                }
                int ready = wait_fd(socket_fd, 0, remaining);
                if (ready == H2_PAL_ERR_WOULD_BLOCK) {
                    continue;
                }
                if (ready != H2_PAL_OK) {
                    (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
                    tls_release_slot(tls_slot);
                    return ready;
                }
            }
            desktop_sigpipe_guard_t guard;
            if (!sigpipe_guard_begin(&guard)) {
                (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
                tls_release_slot(tls_slot);
                return H2_PAL_ERR_IO;
            }
            int got = wolfSSL_read(ssl, data, (int)len);
            sigpipe_guard_end(&guard);
            if (got > 0) {
                (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
                tls_release_slot(tls_slot);
                return got;
            }
            int err = wolfSSL_get_error(ssl, got);
            if (err == WOLFSSL_ERROR_WANT_READ ||
                err == WOLFSSL_ERROR_WANT_WRITE) {
                uint32_t remaining = timeout_remaining_ms(deadline);
                if (remaining == 0u) {
                    (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
                    tls_release_slot(tls_slot);
                    return H2_PAL_ERR_TIMEOUT;
                }
                int ready = wait_fd(
                    socket_fd, err == WOLFSSL_ERROR_WANT_WRITE, remaining);
                if (ready == H2_PAL_ERR_WOULD_BLOCK) {
                    continue;
                }
                if (ready != H2_PAL_OK) {
                    (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
                    tls_release_slot(tls_slot);
                    return ready;
                }
                continue;
            }
            (void)pthread_mutex_unlock(&s_tls_sockets[tls_slot].lock);
            tls_release_slot(tls_slot);
            return err == WOLFSSL_ERROR_ZERO_RETURN ? H2_PAL_ERR_CLOSED
                                                    : H2_PAL_ERR_IO;
        }
    }
    int ready = wait_fd(socket_fd, 0, timeout_ms);
    if (ready != H2_PAL_OK) {
        return ready;
    }
    ssize_t got = recv(socket_fd, data, len, 0);
    if (got == 0) {
        return H2_PAL_ERR_CLOSED;
    }
    return got < 0 ? desktop_net_socket_error() : (int)got;
}

static h2_pal_result_t desktop_net_tls_wrap(
    void *user,
    h2_pal_net_socket_t tcp_socket,
    const h2_pal_net_tls_config_t *config,
    uint32_t timeout_ms,
    h2_pal_net_socket_t *out_tls_socket) {
    (void)user;
    if (tcp_socket < 0 || config == NULL || out_tls_socket == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_tls_socket = -1;
    (void)pthread_mutex_lock(&s_tls_lock);
    if (tls_find_slot(tcp_socket) >= 0) {
        (void)pthread_mutex_unlock(&s_tls_lock);
        return H2_PAL_ERR_INVALID_STATE;
    }
    int slot = tls_find_free_slot();
    if (slot < 0) {
        (void)pthread_mutex_unlock(&s_tls_lock);
        return H2_PAL_ERR_NO_SPACE;
    }
    if (pthread_mutex_init(&s_tls_sockets[slot].lock, NULL) != 0) {
        (void)pthread_mutex_unlock(&s_tls_lock);
        return H2_PAL_ERR_IO;
    }
    s_tls_sockets[slot].in_use = 1;
    s_tls_sockets[slot].closing = 0;
    s_tls_sockets[slot].generation = ++s_tls_generation;
    s_tls_sockets[slot].refs = 0u;
    s_tls_sockets[slot].fd = tcp_socket;
    s_tls_sockets[slot].ssl = NULL;
    s_tls_sockets[slot].ctx = NULL;
    uint32_t slot_generation = s_tls_sockets[slot].generation;
    (void)pthread_mutex_unlock(&s_tls_lock);

    WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (ctx == NULL) {
        (void)pthread_mutex_lock(&s_tls_lock);
        tls_clear_reserved_slot(slot, slot_generation);
        (void)pthread_mutex_unlock(&s_tls_lock);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (wolfSSL_CTX_SetMinVersion(ctx, WOLFSSL_TLSV1_2) != WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(ctx);
        (void)pthread_mutex_lock(&s_tls_lock);
        tls_clear_reserved_slot(slot, slot_generation);
        (void)pthread_mutex_unlock(&s_tls_lock);
        return H2_PAL_ERR_IO;
    }

    h2_pal_net_tls_verify_t verify_mode = config->verify;
    if (verify_mode == H2_PAL_NET_TLS_VERIFY_DEFAULT) {
        verify_mode = H2_PAL_NET_TLS_VERIFY_REQUIRED;
    }
    if (verify_mode == H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY) {
        wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);
    } else if (verify_mode == H2_PAL_NET_TLS_VERIFY_REQUIRED) {
        if (config->server_name == NULL || config->server_name[0] == '\0') {
            wolfSSL_CTX_free(ctx);
            (void)pthread_mutex_lock(&s_tls_lock);
            tls_clear_reserved_slot(slot, slot_generation);
            (void)pthread_mutex_unlock(&s_tls_lock);
            return H2_PAL_ERR_INVALID_ARG;
        }
        h2_pal_result_t ca_rc = tls_load_root_ca(ctx, config);
        if (ca_rc != H2_PAL_OK) {
            wolfSSL_CTX_free(ctx);
            (void)pthread_mutex_lock(&s_tls_lock);
            tls_clear_reserved_slot(slot, slot_generation);
            (void)pthread_mutex_unlock(&s_tls_lock);
            return ca_rc;
        }
        wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, NULL);
    } else {
        wolfSSL_CTX_free(ctx);
        (void)pthread_mutex_lock(&s_tls_lock);
        tls_clear_reserved_slot(slot, slot_generation);
        (void)pthread_mutex_unlock(&s_tls_lock);
        return H2_PAL_ERR_INVALID_ARG;
    }

    WOLFSSL *ssl = wolfSSL_new(ctx);
    if (ssl == NULL) {
        wolfSSL_CTX_free(ctx);
        (void)pthread_mutex_lock(&s_tls_lock);
        tls_clear_reserved_slot(slot, slot_generation);
        (void)pthread_mutex_unlock(&s_tls_lock);
        return H2_PAL_ERR_NO_MEMORY;
    }
    int supported_groups[] = {
        WOLFSSL_ECC_X25519,
        WOLFSSL_ECC_SECP256R1,
    };
    if (wolfSSL_set_groups(
            ssl,
            supported_groups,
            (int)(sizeof(supported_groups) / sizeof(supported_groups[0]))) !=
        WOLFSSL_SUCCESS) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ctx);
        (void)pthread_mutex_lock(&s_tls_lock);
        tls_clear_reserved_slot(slot, slot_generation);
        (void)pthread_mutex_unlock(&s_tls_lock);
        return H2_PAL_ERR_IO;
    }
    if (config->server_name != NULL && config->server_name[0] != '\0') {
        size_t server_name_len = strlen(config->server_name);
        if (server_name_len > (size_t)UINT16_MAX ||
            wolfSSL_UseSNI(
                ssl,
                WOLFSSL_SNI_HOST_NAME,
                config->server_name,
                (unsigned short)server_name_len) != WOLFSSL_SUCCESS) {
            wolfSSL_free(ssl);
            wolfSSL_CTX_free(ctx);
            (void)pthread_mutex_lock(&s_tls_lock);
            tls_clear_reserved_slot(slot, slot_generation);
            (void)pthread_mutex_unlock(&s_tls_lock);
            return H2_PAL_ERR_IO;
        }
        if (wolfSSL_check_domain_name(ssl, config->server_name) !=
            WOLFSSL_SUCCESS) {
            wolfSSL_free(ssl);
            wolfSSL_CTX_free(ctx);
            (void)pthread_mutex_lock(&s_tls_lock);
            tls_clear_reserved_slot(slot, slot_generation);
            (void)pthread_mutex_unlock(&s_tls_lock);
            return H2_PAL_ERR_IO;
        }
    }
    h2_pal_result_t alpn_rc = tls_configure_alpn(ssl, config->alpn);
    if (alpn_rc != H2_PAL_OK) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ctx);
        (void)pthread_mutex_lock(&s_tls_lock);
        tls_clear_reserved_slot(slot, slot_generation);
        (void)pthread_mutex_unlock(&s_tls_lock);
        return alpn_rc;
    }
    if (wolfSSL_set_fd(ssl, tcp_socket) != WOLFSSL_SUCCESS) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ctx);
        (void)pthread_mutex_lock(&s_tls_lock);
        tls_clear_reserved_slot(slot, slot_generation);
        (void)pthread_mutex_unlock(&s_tls_lock);
        return H2_PAL_ERR_IO;
    }

    int flags = fcntl(tcp_socket, F_GETFL, 0);
    if (flags < 0 || fcntl(tcp_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ctx);
        (void)pthread_mutex_lock(&s_tls_lock);
        tls_clear_reserved_slot(slot, slot_generation);
        (void)pthread_mutex_unlock(&s_tls_lock);
        return H2_PAL_ERR_IO;
    }
    h2_pal_result_t rc = tls_wait_handshake(ssl, tcp_socket, timeout_ms, verify_mode);
    if (rc != H2_PAL_OK) {
        (void)fcntl(tcp_socket, F_SETFL, flags);
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ctx);
        (void)pthread_mutex_lock(&s_tls_lock);
        tls_clear_reserved_slot(slot, slot_generation);
        (void)pthread_mutex_unlock(&s_tls_lock);
        return rc;
    }

    (void)pthread_mutex_lock(&s_tls_lock);
    if (!s_tls_sockets[slot].in_use || s_tls_sockets[slot].generation != slot_generation || s_tls_sockets[slot].closing) {
        (void)pthread_mutex_unlock(&s_tls_lock);
        wolfSSL_free(ssl);
        wolfSSL_CTX_free(ctx);
        return H2_PAL_ERR_CLOSED;
    }
    s_tls_sockets[slot].ssl = ssl;
    s_tls_sockets[slot].ctx = ctx;
    (void)pthread_mutex_unlock(&s_tls_lock);
    *out_tls_socket = tcp_socket;
    return H2_PAL_OK;
}

static void desktop_net_close(void *user, h2_pal_net_socket_t socket_fd) {
    (void)user;
    if (socket_fd >= 0) {
        WOLFSSL *ssl = NULL;
        WOLFSSL_CTX *ctx = NULL;
        (void)pthread_mutex_lock(&s_tls_lock);
        int tls_slot = tls_find_slot(socket_fd);
        if (tls_slot >= 0) {
            if (s_tls_sockets[tls_slot].refs == 0u) {
                tls_detach_slot_locked(tls_slot, &ssl, &ctx);
            } else {
                s_tls_sockets[tls_slot].closing = 1;
                s_tls_sockets[tls_slot].fd = -1;
            }
        }
        (void)pthread_mutex_unlock(&s_tls_lock);
        tls_free_handles(ssl, ctx);
        close(socket_fd);
    }
}

static int desktop_net_tcp_listen(
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
    int fd = socket(family_to_posix(family), SOCK_STREAM, 0);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_storage storage;
    socklen_t len = 0;
    int rc = addr_to_sockaddr(&bind_addr, &storage, &len);
    if (rc != H2_PAL_OK) {
        close(fd);
        return rc;
    }
    if (bind(fd, (struct sockaddr *)&storage, len) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    if (getsockname(fd, (struct sockaddr *)&storage, &len) < 0) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    (void)sockaddr_to_addr((const struct sockaddr *)&storage, out_bind_addr);
    *out_socket = fd;
    return H2_PAL_OK;
}

static h2_pal_result_t desktop_net_tcp_accept(
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
    socklen_t len = sizeof(storage);
    int fd = accept(listen_fd, (struct sockaddr *)&storage, &len);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
            errno == ECONNABORTED) {
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

const h2_pal_net_api_t *h2_posix_net_api(void) {
    static const h2_pal_net_vtable_t vtable = {
        .resolve_addr = desktop_net_resolve_addr,
        .resolve_start = desktop_net_resolve_start,
        .resolve_poll = desktop_net_resolve_poll,
        .resolve_close = desktop_net_resolve_close,
        .get_host_addr = desktop_net_get_host_addr,
        .udp_open = desktop_net_udp_open,
        .udp_sendto = desktop_net_udp_sendto,
        .udp_recvfrom = desktop_net_udp_recvfrom,
        .udp_open_bound = desktop_net_udp_open_bound,
        .udp_join_multicast = desktop_net_udp_join_multicast,
        .tcp_open = desktop_net_tcp_open,
        .tcp_open_bound = desktop_net_tcp_open_bound,
        .tcp_connect = desktop_net_tcp_connect,
        .tcp_send = desktop_net_tcp_send,
        .tcp_send_timeout = desktop_net_tcp_send_timeout,
        .tcp_recv = desktop_net_tcp_recv,
        .tls_wrap = desktop_net_tls_wrap,
        .close = desktop_net_close,
        .tcp_listen = desktop_net_tcp_listen,
        .tcp_accept = desktop_net_tcp_accept,
    };
    static const h2_pal_net_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
