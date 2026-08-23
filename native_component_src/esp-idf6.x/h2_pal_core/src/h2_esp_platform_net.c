#include "h2_esp_platform_core.h"
#include "h2_esp_platform_net_socket.h"

#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "ping/ping_sock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_netif.h"
#include "esp_crt_bundle.h"

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/version.h>
#include <psa/crypto.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define H2_ESP_NET_TLS_SOCKET_MAX 8u
#define H2_ESP_NET_TLS_ALPN_MAX 2u
#define H2_ESP_NET_TLS_ALPN_LEN 16u
#define H2_ESP_NET_RESOLVER_MAX 8u

typedef enum esp_net_tls_socket_state {
    ESP_NET_TLS_SOCKET_FREE = 0,
    ESP_NET_TLS_SOCKET_ACTIVE,
    ESP_NET_TLS_SOCKET_CONFIGURING,
    ESP_NET_TLS_SOCKET_CLOSING,
} esp_net_tls_socket_state_t;

typedef struct esp_net_tls_socket {
    esp_net_tls_socket_state_t state;
    h2_pal_net_socket_t fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    char alpn_storage[H2_ESP_NET_TLS_ALPN_MAX][H2_ESP_NET_TLS_ALPN_LEN];
    const char *alpn[H2_ESP_NET_TLS_ALPN_MAX + 1u];
} esp_net_tls_socket_t;

typedef struct esp_net_tls_sync {
    SemaphoreHandle_t registry_mutex;
    SemaphoreHandle_t io_mutexes[H2_ESP_NET_TLS_SOCKET_MAX];
    uint32_t generations[H2_ESP_NET_TLS_SOCKET_MAX];
    esp_net_tls_socket_t sockets[H2_ESP_NET_TLS_SOCKET_MAX];
} esp_net_tls_sync_t;

typedef struct esp_net_resolver {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    int completed;
    int closed;
    h2_pal_result_t result;
    h2_pal_net_addr_t addr;
    char host[];
} esp_net_resolver_t;

static StaticSemaphore_t s_esp_tls_mutex_storage;
static esp_net_tls_sync_t *s_esp_tls_sync;
static esp_net_tls_socket_t *s_esp_tls_sockets;
static size_t s_esp_resolver_count;

static uint64_t esp_net_now_ms(void);
static uint32_t esp_net_timeout_remaining_ms(uint64_t deadline_ms);

static void esp_net_tls_init(void) {
    if (s_esp_tls_sync == NULL) {
        esp_net_tls_sync_t *sync = h2_pal_mem_alloc(
            h2_esp_platform_default_allocator(), sizeof(*sync));
        if (sync == NULL) {
            return;
        }
        memset(sync, 0, sizeof(*sync));
        sync->registry_mutex =
            xSemaphoreCreateMutexStatic(&s_esp_tls_mutex_storage);
        int ready = sync->registry_mutex != NULL;
        for (size_t index = 0u; index < H2_ESP_NET_TLS_SOCKET_MAX; ++index) {
            sync->sockets[index].fd = -1;
            sync->io_mutexes[index] = xSemaphoreCreateMutex();
            ready = ready && sync->io_mutexes[index] != NULL;
        }
        if (!ready) {
            for (size_t index = 0u; index < H2_ESP_NET_TLS_SOCKET_MAX; ++index) {
                if (sync->io_mutexes[index] != NULL) {
                    vSemaphoreDelete(sync->io_mutexes[index]);
                }
            }
            if (sync->registry_mutex != NULL) {
                vSemaphoreDelete(sync->registry_mutex);
            }
            h2_pal_mem_free(h2_esp_platform_default_allocator(), sync);
            return;
        }
        s_esp_tls_sockets = sync->sockets;
        s_esp_tls_sync = sync;
    }
}

static int esp_net_tls_find(h2_pal_net_socket_t fd) {
    for (size_t index = 0u; index < H2_ESP_NET_TLS_SOCKET_MAX; ++index) {
        if (s_esp_tls_sockets[index].state != ESP_NET_TLS_SOCKET_FREE &&
            s_esp_tls_sockets[index].fd == fd) {
            return (int)index;
        }
    }
    return -1;
}

static int esp_net_tls_find_free(void) {
    for (size_t index = 0u; index < H2_ESP_NET_TLS_SOCKET_MAX; ++index) {
        if (s_esp_tls_sockets[index].state == ESP_NET_TLS_SOCKET_FREE) {
            return (int)index;
        }
    }
    return -1;
}

static TickType_t esp_net_tls_timeout_ticks(uint32_t timeout_ms) {
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return timeout_ms != 0u && ticks == 0u ? 1u : ticks;
}

static int esp_net_tls_take_mutex(
    SemaphoreHandle_t mutex,
    int bounded,
    uint32_t timeout_ms,
    uint64_t deadline_ms) {
    if (mutex == NULL) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    TickType_t ticks = portMAX_DELAY;
    if (bounded) {
        uint32_t remaining_ms = timeout_ms == 0u
            ? 0u
            : esp_net_timeout_remaining_ms(deadline_ms);
        if (timeout_ms != 0u && remaining_ms == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        ticks = esp_net_tls_timeout_ticks(remaining_ms);
    }
    if (xSemaphoreTake(mutex, ticks) == pdTRUE) {
        return H2_PAL_OK;
    }
    return bounded && timeout_ms == 0u
        ? H2_PAL_ERR_WOULD_BLOCK
        : H2_PAL_ERR_TIMEOUT;
}

static int esp_net_tls_acquire(
    h2_pal_net_socket_t fd,
    int bounded,
    uint32_t timeout_ms,
    uint64_t deadline_ms,
    esp_net_tls_socket_t **out_socket,
    size_t *out_index) {
    if (s_esp_tls_sync == NULL) {
        return 0;
    }
    int rc = esp_net_tls_take_mutex(
        s_esp_tls_sync->registry_mutex, bounded, timeout_ms, deadline_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    int slot_index = esp_net_tls_find(fd);
    if (slot_index < 0) {
        (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
        return 0;
    }
    esp_net_tls_socket_t *slot = &s_esp_tls_sockets[slot_index];
    if (slot->state != ESP_NET_TLS_SOCKET_ACTIVE) {
        (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
        return H2_PAL_ERR_CLOSED;
    }
    uint32_t generation = s_esp_tls_sync->generations[slot_index];
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);

    rc = esp_net_tls_take_mutex(
        s_esp_tls_sync->io_mutexes[slot_index], bounded,
        timeout_ms, deadline_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = esp_net_tls_take_mutex(
        s_esp_tls_sync->registry_mutex, bounded, timeout_ms, deadline_ms);
    if (rc != H2_PAL_OK) {
        (void)xSemaphoreGive(s_esp_tls_sync->io_mutexes[slot_index]);
        return rc;
    }
    if (slot->state != ESP_NET_TLS_SOCKET_ACTIVE || slot->fd != fd ||
        s_esp_tls_sync->generations[slot_index] != generation) {
        (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
        (void)xSemaphoreGive(s_esp_tls_sync->io_mutexes[slot_index]);
        return H2_PAL_ERR_CLOSED;
    }
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    *out_socket = slot;
    *out_index = (size_t)slot_index;
    return 1;
}

static void esp_net_tls_release(size_t slot_index) {
    (void)xSemaphoreGive(s_esp_tls_sync->io_mutexes[slot_index]);
}

#if MBEDTLS_VERSION_MAJOR < 4
static int esp_net_tls_random(void *user, unsigned char *out, size_t len) {
    (void)user;
    return h2_pal_crypto_random(h2_esp_platform_crypto_api(), out, len) ==
            H2_PAL_OK
        ? 0
        : -1;
}
#endif

static int esp_net_tls_send_raw(
    void *user, const unsigned char *data, size_t len) {
    esp_net_tls_socket_t *socket = (esp_net_tls_socket_t *)user;
    int sent = send(socket->fd, data, len, MSG_DONTWAIT);
    if (sent >= 0) {
        return sent;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
        ? MBEDTLS_ERR_SSL_WANT_WRITE
        : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int esp_net_tls_recv_raw(
    void *user, unsigned char *data, size_t len) {
    esp_net_tls_socket_t *socket = (esp_net_tls_socket_t *)user;
    int received = recv(socket->fd, data, len, MSG_DONTWAIT);
    if (received >= 0) {
        return received;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR
        ? MBEDTLS_ERR_SSL_WANT_READ
        : MBEDTLS_ERR_NET_RECV_FAILED;
}

static void esp_net_tls_free(esp_net_tls_socket_t *socket) {
    if (socket == NULL || socket->state == ESP_NET_TLS_SOCKET_FREE) {
        return;
    }
    mbedtls_ssl_free(&socket->ssl);
    mbedtls_ssl_config_free(&socket->config);
    mbedtls_x509_crt_free(&socket->ca);
    memset(socket, 0, sizeof(*socket));
    socket->fd = -1;
}

static int esp_net_tls_wait(
    esp_net_tls_socket_t *socket, int result, uint32_t timeout_ms) {
    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        return h2_esp_net_wait_fd(socket->fd, 0, timeout_ms);
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return h2_esp_net_wait_fd(socket->fd, 1, timeout_ms);
    }
    return H2_PAL_ERR_IO;
}

static h2_pal_result_t esp_net_tls_copy_alpn(
    esp_net_tls_socket_t *socket,
    const h2_pal_net_tls_config_t *config) {
    if (config->alpn == NULL || config->alpn[0] == '\0') {
        return H2_PAL_OK;
    }
    if (strlen(config->alpn) >= H2_ESP_NET_TLS_ALPN_LEN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)strcpy(socket->alpn_storage[0], config->alpn);
    socket->alpn[0] = socket->alpn_storage[0];
    return H2_PAL_OK;
}

static h2_pal_result_t esp_net_tls_load_ca(
    esp_net_tls_socket_t *socket,
    const h2_pal_net_tls_config_t *config) {
    if (config->verify == H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY) {
        mbedtls_ssl_conf_authmode(&socket->config, MBEDTLS_SSL_VERIFY_NONE);
        return H2_PAL_OK;
    }
    mbedtls_ssl_conf_authmode(&socket->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    if (config->root_ca_pem == NULL || config->root_ca_pem_len == 0u) {
        return esp_crt_bundle_attach(&socket->config) == ESP_OK
            ? H2_PAL_OK
            : H2_PAL_ERR_UNAVAILABLE;
    }
    if (config->root_ca_pem_len == SIZE_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint8_t *pem = h2_pal_mem_alloc(
        h2_esp_platform_default_allocator(), config->root_ca_pem_len + 1u);
    if (pem == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(pem, config->root_ca_pem, config->root_ca_pem_len);
    pem[config->root_ca_pem_len] = '\0';
    int result = mbedtls_x509_crt_parse(
        &socket->ca, pem, config->root_ca_pem_len + 1u);
    h2_pal_mem_free(h2_esp_platform_default_allocator(), pem);
    if (result != 0) {
        return H2_PAL_ERR_FORMAT;
    }
    mbedtls_ssl_conf_ca_chain(&socket->config, &socket->ca, NULL);
    return H2_PAL_OK;
}

static h2_pal_result_t esp_net_tls_handshake(
    esp_net_tls_socket_t *socket, uint32_t timeout_ms) {
    uint64_t deadline = esp_net_now_ms() + timeout_ms;
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
        uint32_t remaining = esp_net_timeout_remaining_ms(deadline);
        if (remaining == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        int wait_result = esp_net_tls_wait(socket, result, remaining);
        if (wait_result != H2_PAL_OK && wait_result != H2_PAL_ERR_WOULD_BLOCK) {
            return wait_result;
        }
    }
}

static int family_to_lwip(h2_pal_net_family_t family) {
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

static int sockaddr_to_addr(const struct sockaddr *sockaddr, h2_pal_net_addr_t *out_addr) {
    if (sockaddr == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_addr, 0, sizeof(*out_addr));
    if (sockaddr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sockaddr;
        out_addr->family = H2_PAL_NET_FAMILY_IPV6;
        out_addr->port = ntohs(sin6->sin6_port);
        memcpy(out_addr->ip, &sin6->sin6_addr, 16u);
        return H2_PAL_OK;
    }
    if (sockaddr->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sockaddr;
        out_addr->family = H2_PAL_NET_FAMILY_IPV4;
        out_addr->port = ntohs(sin->sin_port);
        memcpy(out_addr->ip, &sin->sin_addr, 4u);
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static void set_recv_timeout(int fd, uint32_t timeout_ms) {
    struct timeval timeout;
    timeout.tv_sec = (long)(timeout_ms / 1000u);
    timeout.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

static int esp_net_socket_error(void) {
    return errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN
        ? H2_PAL_ERR_CLOSED
        : H2_PAL_ERR_IO;
}

static uint64_t esp_net_now_ms(void) {
    struct timeval now;
    return gettimeofday(&now, NULL) == 0
        ? (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_usec / 1000u
        : 0u;
}

static uint32_t esp_net_timeout_remaining_ms(uint64_t deadline_ms) {
    uint64_t now_ms = esp_net_now_ms();
    return now_ms >= deadline_ms
        ? 0u
        : (uint32_t)(deadline_ms - now_ms);
}

static int esp_net_resolve_host(
    const char *host,
    h2_pal_net_addr_t *out_addr) {
    if (host == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        return H2_PAL_ERR_NOT_FOUND;
    }
    int out_rc = H2_PAL_ERR_NOT_FOUND;
    for (struct addrinfo *it = res; it != NULL; it = it->ai_next) {
        out_rc = sockaddr_to_addr(it->ai_addr, out_addr);
        if (out_rc == H2_PAL_OK) {
            break;
        }
    }
    freeaddrinfo(res);
    return out_rc;
}

static int esp_net_resolve_addr(
    void *user,
    const char *host,
    h2_pal_net_addr_t *out_addr) {
    (void)user;
    return esp_net_resolve_host(host, out_addr);
}

static h2_pal_result_t esp_net_resolver_reserve(void) {
    if (s_esp_tls_sync == NULL ||
        xSemaphoreTake(
            s_esp_tls_sync->registry_mutex, portMAX_DELAY) != pdTRUE) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    h2_pal_result_t result =
        s_esp_resolver_count < H2_ESP_NET_RESOLVER_MAX
            ? H2_PAL_OK
            : H2_PAL_ERR_NO_SPACE;
    if (result == H2_PAL_OK) {
        s_esp_resolver_count += 1u;
    }
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    return result;
}

static void esp_net_resolver_release(void) {
    if (s_esp_tls_sync == NULL ||
        xSemaphoreTake(
            s_esp_tls_sync->registry_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_esp_resolver_count > 0u) {
        s_esp_resolver_count -= 1u;
    }
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
}

static void esp_net_resolver_destroy(esp_net_resolver_t *resolver) {
    if (resolver == NULL) {
        return;
    }
    if (resolver->done != NULL) {
        vSemaphoreDelete(resolver->done);
    }
    if (resolver->lock != NULL) {
        vSemaphoreDelete(resolver->lock);
    }
    h2_pal_mem_free(h2_esp_platform_default_allocator(), resolver);
    esp_net_resolver_release();
}

static void esp_net_resolver_worker(void *raw) {
    esp_net_resolver_t *resolver = (esp_net_resolver_t *)raw;
    h2_pal_net_addr_t addr;
    h2_pal_result_t result = esp_net_resolve_host(resolver->host, &addr);
    (void)xSemaphoreTake(resolver->lock, portMAX_DELAY);
    resolver->result = result;
    if (result == H2_PAL_OK) {
        resolver->addr = addr;
    }
    resolver->completed = 1;
    int closed = resolver->closed;
    if (!closed) {
        (void)xSemaphoreGive(resolver->done);
    }
    (void)xSemaphoreGive(resolver->lock);
    if (closed) {
        esp_net_resolver_destroy(resolver);
    }
    vTaskDelete(NULL);
}

static h2_pal_result_t esp_net_resolve_start(
    void *user,
    const char *host,
    h2_pal_net_resolver_t **out_resolver) {
    (void)user;
    if (host == NULL || host[0] == '\0' || out_resolver == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_resolver = NULL;
    h2_pal_result_t reserve_result = esp_net_resolver_reserve();
    if (reserve_result != H2_PAL_OK) {
        return reserve_result;
    }
    size_t host_len = strlen(host);
    if (host_len >= SIZE_MAX - sizeof(esp_net_resolver_t)) {
        esp_net_resolver_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    esp_net_resolver_t *resolver = (esp_net_resolver_t *)h2_pal_mem_alloc(
        h2_esp_platform_default_allocator(),
        sizeof(*resolver) + host_len + 1u);
    if (resolver == NULL) {
        esp_net_resolver_release();
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(resolver, 0, sizeof(*resolver));
    memcpy(resolver->host, host, host_len + 1u);
    resolver->lock = xSemaphoreCreateMutex();
    resolver->done = xSemaphoreCreateBinary();
    if (resolver->lock == NULL || resolver->done == NULL) {
        esp_net_resolver_destroy(resolver);
        return H2_PAL_ERR_NO_MEMORY;
    }
    if (xTaskCreate(
            esp_net_resolver_worker, "h2_dns", 4096u, resolver,
            tskIDLE_PRIORITY + 1u, NULL) != pdPASS) {
        esp_net_resolver_destroy(resolver);
        return H2_PAL_ERR_NO_MEMORY;
    }
    *out_resolver = (h2_pal_net_resolver_t *)resolver;
    return H2_PAL_OK;
}

static h2_pal_result_t esp_net_resolve_poll(
    void *user,
    h2_pal_net_resolver_t *resolver_handle,
    h2_pal_net_addr_t *out_addr,
    uint32_t timeout_ms) {
    (void)user;
    esp_net_resolver_t *resolver = (esp_net_resolver_t *)resolver_handle;
    if (resolver == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)xSemaphoreTake(resolver->lock, portMAX_DELAY);
    if (resolver->closed) {
        (void)xSemaphoreGive(resolver->lock);
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (resolver->completed) {
        h2_pal_result_t result = resolver->result;
        if (result == H2_PAL_OK) {
            *out_addr = resolver->addr;
        }
        (void)xSemaphoreGive(resolver->lock);
        return result;
    }
    (void)xSemaphoreGive(resolver->lock);

    TickType_t ticks = timeout_ms == 0u
        ? 0u
        : esp_net_tls_timeout_ticks(timeout_ms);
    BaseType_t wait_result = xSemaphoreTake(resolver->done, ticks);
    (void)xSemaphoreTake(resolver->lock, portMAX_DELAY);
    if (!resolver->completed) {
        (void)xSemaphoreGive(resolver->lock);
        return timeout_ms == 0u || wait_result == pdTRUE
            ? H2_PAL_ERR_WOULD_BLOCK
            : H2_PAL_ERR_TIMEOUT;
    }
    h2_pal_result_t result = resolver->result;
    if (result == H2_PAL_OK) {
        *out_addr = resolver->addr;
    }
    (void)xSemaphoreGive(resolver->lock);
    return result;
}

static void esp_net_resolve_close(
    void *user,
    h2_pal_net_resolver_t *resolver_handle) {
    (void)user;
    esp_net_resolver_t *resolver = (esp_net_resolver_t *)resolver_handle;
    if (resolver == NULL) {
        return;
    }
    (void)xSemaphoreTake(resolver->lock, portMAX_DELAY);
    resolver->closed = 1;
    int completed = resolver->completed;
    (void)xSemaphoreGive(resolver->lock);
    if (completed) {
        esp_net_resolver_destroy(resolver);
    }
}

typedef struct esp_net_host_addr_context {
    const char *iface_prefix;
    h2_pal_net_addr_t addr;
    h2_pal_result_t result;
} esp_net_host_addr_context_t;

static int esp_net_iface_matches_prefix(
    esp_netif_t *netif,
    const char *prefix) {
    if (netif == NULL) {
        return 0;
    }
    if (prefix == NULL || prefix[0] == '\0') {
        return 1;
    }
    const size_t prefix_len = strlen(prefix);
    char impl_name[7] = {0};
    if (prefix_len < sizeof(impl_name) &&
        esp_netif_get_netif_impl_name(netif, impl_name) == ESP_OK &&
        strncmp(impl_name, prefix, prefix_len) == 0) {
        return 1;
    }
    const char *ifkey = esp_netif_get_ifkey(netif);
    if (ifkey != NULL && strncmp(ifkey, prefix, prefix_len) == 0) {
        return 1;
    }
    const char *description = esp_netif_get_desc(netif);
    return description != NULL &&
           strncmp(description, prefix, prefix_len) == 0;
}

static int esp_net_read_ipv4(
    esp_netif_t *netif,
    h2_pal_net_addr_t *out_addr) {
    esp_netif_ip_info_t info;
    memset(&info, 0, sizeof(info));
    if (netif == NULL || !esp_netif_is_netif_up(netif) ||
        esp_netif_get_ip_info(netif, &info) != ESP_OK ||
        info.ip.addr == 0u) {
        return 0;
    }
    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->family = H2_PAL_NET_FAMILY_IPV4;
    memcpy(out_addr->ip, &info.ip.addr, 4u);
    return 1;
}

static esp_err_t esp_net_get_host_addr_in_tcpip(void *user) {
    esp_net_host_addr_context_t *context =
        (esp_net_host_addr_context_t *)user;
    esp_netif_t *default_netif = esp_netif_get_default_netif();
    if (esp_net_iface_matches_prefix(
            default_netif, context->iface_prefix) &&
        esp_net_read_ipv4(default_netif, &context->addr)) {
        context->result = H2_PAL_OK;
        return ESP_OK;
    }

    esp_netif_t *netif = NULL;
    while ((netif = esp_netif_next_unsafe(netif)) != NULL) {
        if (netif != default_netif &&
            esp_net_iface_matches_prefix(netif, context->iface_prefix) &&
            esp_net_read_ipv4(netif, &context->addr)) {
            context->result = H2_PAL_OK;
            return ESP_OK;
        }
    }
    context->result = H2_PAL_ERR_UNAVAILABLE;
    return ESP_OK;
}

static int esp_net_get_host_addr(
    void *user,
    const char *iface_prefix,
    h2_pal_net_addr_t *out_addr) {
    (void)user;
    if (out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_addr, 0, sizeof(*out_addr));
    esp_net_host_addr_context_t context = {
        .iface_prefix = iface_prefix,
        .result = H2_PAL_ERR_UNAVAILABLE,
    };
    if (esp_netif_tcpip_exec(
            esp_net_get_host_addr_in_tcpip, &context) != ESP_OK) {
        return H2_PAL_ERR_IO;
    }
    if (context.result == H2_PAL_OK) {
        *out_addr = context.addr;
    }
    return context.result;
}

static int esp_net_udp_open(
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
    int fd = socket(family_to_lwip(family), SOCK_DGRAM, 0);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    h2_pal_net_addr_t bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.family = family;
    bind_addr.port = port;
    struct sockaddr_storage storage;
    socklen_t sock_len = 0;
    int rc = addr_to_sockaddr(&bind_addr, &storage, &sock_len);
    if (rc != H2_PAL_OK || bind(fd, (struct sockaddr *)&storage, sock_len) < 0) {
        close(fd);
        return rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc;
    }
    if (getsockname(fd, (struct sockaddr *)&storage, &sock_len) < 0) {
        close(fd);
        return H2_PAL_ERR_IO;
    }
    (void)sockaddr_to_addr((const struct sockaddr *)&storage, out_bind_addr);
    *out_socket = fd;
    return H2_PAL_OK;
}

static int esp_net_udp_sendto(
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
    return h2_esp_net_udp_send_result(sent, errno);
}

static int esp_net_udp_recvfrom(
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
    int ready = h2_esp_net_wait_fd(socket_fd, 0, timeout_ms);
    if (ready != H2_PAL_OK) {
        return ready;
    }
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

static int esp_net_tcp_open(
    void *user,
    h2_pal_net_family_t family,
    h2_pal_net_socket_t *out_socket) {
    (void)user;
    if (out_socket == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_socket = -1;
    int fd = socket(family_to_lwip(family), SOCK_STREAM, 0);
    if (fd < 0) {
        return H2_PAL_ERR_IO;
    }
    *out_socket = fd;
    return H2_PAL_OK;
}

static int esp_net_tcp_open_bound(
    void *user,
    h2_pal_net_family_t family,
    const h2_pal_net_bind_t *bind_config,
    h2_pal_net_socket_t *out_socket) {
    if (bind_config == NULL || bind_config->type == H2_PAL_NET_BIND_DEFAULT) {
        return esp_net_tcp_open(user, family, out_socket);
    }
    if (bind_config->type != H2_PAL_NET_BIND_SOURCE_ADDR ||
        bind_config->source_addr.family != family) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    int result = esp_net_tcp_open(user, family, out_socket);
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
        close(*out_socket);
        *out_socket = -1;
        return result == H2_PAL_OK ? H2_PAL_ERR_IO : result;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t esp_net_tcp_connect(
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
    int result = addr_to_sockaddr(addr, &storage, &sock_len);
    if (result != H2_PAL_OK) {
        return result;
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
        return esp_net_socket_error();
    }
    result = h2_esp_net_wait_fd(socket_fd, 1, timeout_ms);
    if (result != H2_PAL_OK) {
        if (result != H2_PAL_ERR_TIMEOUT &&
            result != H2_PAL_ERR_WOULD_BLOCK) {
            (void)fcntl(socket_fd, F_SETFL, flags);
        }
        return result;
    }
    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    if (getsockopt(
            socket_fd, SOL_SOCKET, SO_ERROR,
            &socket_error, &error_len) < 0 || socket_error != 0) {
        (void)fcntl(socket_fd, F_SETFL, flags);
        errno = socket_error;
        return esp_net_socket_error();
    }
    return fcntl(socket_fd, F_SETFL, flags & ~O_NONBLOCK) == 0
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

static int esp_net_tcp_send(
    void *user,
    h2_pal_net_socket_t socket_fd,
    const uint8_t *data,
    size_t len) {
    (void)user;
    if (socket_fd < 0 || (data == NULL && len != 0u) ||
        len > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    esp_net_tls_socket_t *tls_socket = NULL;
    size_t tls_slot = 0u;
    int tls_result = esp_net_tls_acquire(
        socket_fd, 0, 0u, 0u, &tls_socket, &tls_slot);
    if (tls_result < 0) {
        return tls_result;
    }
    if (tls_result > 0) {
        int result = mbedtls_ssl_write(&tls_socket->ssl, data, len);
        esp_net_tls_release(tls_slot);
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
    return sent < 0 ? esp_net_socket_error() : sent;
}

static int esp_net_tcp_send_timeout(
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
    uint64_t deadline_ms = esp_net_now_ms() + timeout_ms;
    esp_net_tls_socket_t *tls_socket = NULL;
    size_t tls_slot = 0u;
    int tls_result = esp_net_tls_acquire(
        socket_fd, 1, timeout_ms, deadline_ms, &tls_socket, &tls_slot);
    if (tls_result < 0) {
        return tls_result;
    }
    if (tls_result > 0) {
        for (;;) {
            int result = mbedtls_ssl_write(&tls_socket->ssl, data, len);
            if (result > 0) {
                esp_net_tls_release(tls_slot);
                return result;
            }
            if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                result != MBEDTLS_ERR_SSL_WANT_WRITE) {
                esp_net_tls_release(tls_slot);
                return result == 0 ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_IO;
            }
            uint32_t remaining = timeout_ms == 0u
                ? 0u
                : esp_net_timeout_remaining_ms(deadline_ms);
            int wait_result = esp_net_tls_wait(tls_socket, result, remaining);
            if (wait_result == H2_PAL_ERR_TIMEOUT) {
                wait_result = timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                               : H2_PAL_ERR_TIMEOUT;
            }
            if (wait_result != H2_PAL_OK &&
                wait_result != H2_PAL_ERR_WOULD_BLOCK) {
                esp_net_tls_release(tls_slot);
                return wait_result;
            }
            if (wait_result == H2_PAL_ERR_WOULD_BLOCK && timeout_ms == 0u) {
                esp_net_tls_release(tls_slot);
                return H2_PAL_ERR_WOULD_BLOCK;
            }
        }
    }
    for (;;) {
        uint32_t remaining_ms = timeout_ms == 0u
            ? 0u
            : esp_net_timeout_remaining_ms(deadline_ms);
        int ready = h2_esp_net_wait_fd(socket_fd, 1, remaining_ms);
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
            return esp_net_socket_error();
        }
        if (timeout_ms == 0u) {
            return H2_PAL_ERR_WOULD_BLOCK;
        }
        if (esp_net_timeout_remaining_ms(deadline_ms) == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
    }
}

static int esp_net_tcp_recv(
    void *user,
    h2_pal_net_socket_t socket_fd,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    (void)user;
    if (socket_fd < 0 || data == NULL || len == 0u ||
        len > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    uint64_t deadline_ms = esp_net_now_ms() + timeout_ms;
    esp_net_tls_socket_t *tls_socket = NULL;
    size_t tls_slot = 0u;
    int tls_result = esp_net_tls_acquire(
        socket_fd, 1, timeout_ms, deadline_ms, &tls_socket, &tls_slot);
    if (tls_result < 0) {
        return tls_result;
    }
    if (tls_result > 0) {
        for (;;) {
            int result = mbedtls_ssl_read(&tls_socket->ssl, data, len);
            if (result > 0) {
                esp_net_tls_release(tls_slot);
                return result;
            }
            if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                esp_net_tls_release(tls_slot);
                return H2_PAL_ERR_CLOSED;
            }
            if (result != MBEDTLS_ERR_SSL_WANT_READ &&
                result != MBEDTLS_ERR_SSL_WANT_WRITE) {
                esp_net_tls_release(tls_slot);
                return H2_PAL_ERR_IO;
            }
            uint32_t remaining = timeout_ms == 0u
                ? 0u
                : esp_net_timeout_remaining_ms(deadline_ms);
            int wait_result = esp_net_tls_wait(tls_socket, result, remaining);
            if (wait_result == H2_PAL_ERR_TIMEOUT) {
                wait_result = timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                               : H2_PAL_ERR_TIMEOUT;
            }
            if (wait_result != H2_PAL_OK &&
                wait_result != H2_PAL_ERR_WOULD_BLOCK) {
                esp_net_tls_release(tls_slot);
                return wait_result;
            }
            if (wait_result == H2_PAL_ERR_WOULD_BLOCK && timeout_ms == 0u) {
                esp_net_tls_release(tls_slot);
                return H2_PAL_ERR_WOULD_BLOCK;
            }
        }
    }
    set_recv_timeout(socket_fd, timeout_ms);
    int received = recv(socket_fd, data, (int)len, 0);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                    : H2_PAL_ERR_TIMEOUT;
        }
        return esp_net_socket_error();
    }
    return received == 0 ? H2_PAL_ERR_CLOSED : received;
}

static void esp_net_close(void *user, h2_pal_net_socket_t socket_fd) {
    (void)user;
    if (socket_fd >= 0) {
        if (s_esp_tls_sync != NULL &&
            xSemaphoreTake(
                s_esp_tls_sync->registry_mutex, portMAX_DELAY) == pdTRUE) {
            int slot_index = esp_net_tls_find(socket_fd);
            if (slot_index >= 0) {
                esp_net_tls_socket_t *slot = &s_esp_tls_sockets[slot_index];
                uint32_t generation =
                    s_esp_tls_sync->generations[slot_index];
                slot->state = ESP_NET_TLS_SOCKET_CLOSING;
                (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
                if (xSemaphoreTake(
                        s_esp_tls_sync->io_mutexes[slot_index],
                        portMAX_DELAY) ==
                    pdTRUE) {
                    if (xSemaphoreTake(
                            s_esp_tls_sync->registry_mutex,
                            portMAX_DELAY) == pdTRUE) {
                        if (slot->state == ESP_NET_TLS_SOCKET_CLOSING &&
                            slot->fd == socket_fd &&
                            s_esp_tls_sync->generations[slot_index] ==
                                generation) {
                            esp_net_tls_free(slot);
                            s_esp_tls_sync->generations[slot_index] += 1u;
                        }
                        (void)xSemaphoreGive(
                            s_esp_tls_sync->registry_mutex);
                    }
                    esp_net_tls_release((size_t)slot_index);
                }
            } else {
                (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
            }
        }
        close(socket_fd);
    }
}

static h2_pal_result_t esp_net_tls_wrap(
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
    esp_net_tls_init();
    uint64_t deadline_ms = esp_net_now_ms() + timeout_ms;
    int lock_result = esp_net_tls_take_mutex(
        s_esp_tls_sync == NULL ? NULL : s_esp_tls_sync->registry_mutex,
        1, timeout_ms, deadline_ms);
    if (lock_result != H2_PAL_OK) {
        return lock_result;
    }
    if (esp_net_tls_find(socket_fd) >= 0) {
        (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
        return H2_PAL_ERR_INVALID_STATE;
    }
    int slot_index = esp_net_tls_find_free();
    if (slot_index < 0) {
        (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
        return H2_PAL_ERR_NO_MEMORY;
    }
    esp_net_tls_socket_t *slot = &s_esp_tls_sockets[slot_index];
    if (xSemaphoreTake(s_esp_tls_sync->io_mutexes[slot_index], 0u) != pdTRUE) {
        (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
        return H2_PAL_ERR_UNAVAILABLE;
    }
    memset(slot, 0, sizeof(*slot));
    slot->state = ESP_NET_TLS_SOCKET_CONFIGURING;
    slot->fd = socket_fd;
    s_esp_tls_sync->generations[slot_index] += 1u;
    uint32_t generation = s_esp_tls_sync->generations[slot_index];
    (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    mbedtls_ssl_init(&slot->ssl);
    mbedtls_ssl_config_init(&slot->config);
    mbedtls_x509_crt_init(&slot->ca);
    h2_pal_result_t rc = esp_net_tls_copy_alpn(slot, config);
    int result = rc == H2_PAL_OK
        ? mbedtls_ssl_config_defaults(
              &slot->config, MBEDTLS_SSL_IS_CLIENT,
              MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)
        : -1;
    if (result == 0) {
#if MBEDTLS_VERSION_MAJOR < 4
        mbedtls_ssl_conf_rng(&slot->config, esp_net_tls_random, slot);
#else
        if (psa_crypto_init() != PSA_SUCCESS) {
            rc = H2_PAL_ERR_IO;
        }
#endif
    }
    if (result == 0 && rc == H2_PAL_OK) {
        rc = esp_net_tls_load_ca(slot, config);
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
            &slot->ssl, slot, esp_net_tls_send_raw, esp_net_tls_recv_raw, NULL);
        uint32_t remaining_ms = esp_net_timeout_remaining_ms(deadline_ms);
        rc = remaining_ms == 0u
            ? H2_PAL_ERR_TIMEOUT
            : esp_net_tls_handshake(slot, remaining_ms);
    }
    if (xSemaphoreTake(
            s_esp_tls_sync->registry_mutex, portMAX_DELAY) == pdTRUE) {
        if (slot->state == ESP_NET_TLS_SOCKET_CONFIGURING &&
            slot->fd == socket_fd &&
            s_esp_tls_sync->generations[slot_index] == generation) {
            if (rc != H2_PAL_OK) {
                esp_net_tls_free(slot);
                s_esp_tls_sync->generations[slot_index] += 1u;
            } else {
                slot->state = ESP_NET_TLS_SOCKET_ACTIVE;
                *out_socket = socket_fd;
            }
        } else {
            rc = H2_PAL_ERR_CLOSED;
        }
        (void)xSemaphoreGive(s_esp_tls_sync->registry_mutex);
    } else {
        rc = H2_PAL_ERR_UNAVAILABLE;
    }
    esp_net_tls_release((size_t)slot_index);
    return rc;
}

typedef struct esp_net_ping_context {
    SemaphoreHandle_t done;
    h2_pal_net_icmp_echo_result_t result;
} esp_net_ping_context_t;

static void esp_net_ping_record(esp_ping_handle_t handle, void *args) {
    esp_net_ping_context_t *context = (esp_net_ping_context_t *)args;
    uint32_t elapsed_ms = 0u;
    uint32_t transmitted = 0u;
    uint32_t received = 0u;
    (void)esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    (void)esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    (void)esp_ping_get_profile(handle, ESP_PING_PROF_REPLY, &received, sizeof(received));
    context->result.elapsed_ms = elapsed_ms;
    context->result.transmitted = transmitted;
    context->result.received = received;
}

static void esp_net_ping_success(esp_ping_handle_t handle, void *args) {
    esp_net_ping_record(handle, args);
}

static void esp_net_ping_timeout(esp_ping_handle_t handle, void *args) {
    esp_net_ping_record(handle, args);
}

static void esp_net_ping_end(esp_ping_handle_t handle, void *args) {
    (void)handle;
    esp_net_ping_context_t *context = (esp_net_ping_context_t *)args;
    (void)xSemaphoreGive(context->done);
}

static h2_pal_result_t esp_net_icmp_echo(
    void *user,
    const h2_pal_net_addr_t *addr,
    const h2_pal_net_bind_t *bind,
    uint32_t timeout_ms,
    h2_pal_net_icmp_echo_result_t *out_result) {
    (void)user;
    if (addr == NULL || addr->family != H2_PAL_NET_FAMILY_IPV4 ||
        timeout_ms == 0u || out_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (bind != NULL && bind->type != H2_PAL_NET_BIND_DEFAULT) {
        return H2_PAL_ERR_UNSUPPORTED;
    }

    memset(out_result, 0, sizeof(*out_result));
    esp_net_ping_context_t context = {
        .done = xSemaphoreCreateBinary(),
    };
    if (context.done == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 1u;
    config.timeout_ms = timeout_ms;
    config.interval_ms = 0u;
    config.target_addr.type = IPADDR_TYPE_V4;
    memcpy(&config.target_addr.u_addr.ip4.addr, addr->ip, 4u);
    const esp_ping_callbacks_t callbacks = {
        .on_ping_success = esp_net_ping_success,
        .on_ping_timeout = esp_net_ping_timeout,
        .on_ping_end = esp_net_ping_end,
        .cb_args = &context,
    };
    esp_ping_handle_t handle = NULL;
    esp_err_t err = esp_ping_new_session(&config, &callbacks, &handle);
    if (err == ESP_OK) {
        err = esp_ping_start(handle);
    }
    h2_pal_result_t rc = H2_PAL_ERR_IO;
    if (err == ESP_OK) {
        const TickType_t grace_ticks = pdMS_TO_TICKS(250u);
        const uint64_t timeout_ticks =
            ((uint64_t)timeout_ms * (uint64_t)configTICK_RATE_HZ + 999u) / 1000u;
        TickType_t wait_ticks = timeout_ticks > (uint64_t)(portMAX_DELAY - grace_ticks)
            ? portMAX_DELAY
            : (TickType_t)timeout_ticks + grace_ticks;
        if (xSemaphoreTake(context.done, wait_ticks) == pdTRUE) {
            *out_result = context.result;
            rc = context.result.received > 0u ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
        } else {
            /* The ping task owns the callbacks until on_ping_end runs. Stop the
             * session and wait for that callback before releasing its context. */
            (void)esp_ping_stop(handle);
            (void)xSemaphoreTake(context.done, portMAX_DELAY);
            *out_result = context.result;
            rc = H2_PAL_ERR_TIMEOUT;
        }
    }
    if (handle != NULL) {
        (void)esp_ping_stop(handle);
        (void)esp_ping_delete_session(handle);
    }
    vSemaphoreDelete(context.done);
    return rc;
}

const h2_pal_net_api_t *h2_esp_platform_net_api(void) {
    esp_net_tls_init();
    static const h2_pal_net_vtable_t vtable = {
        .resolve_addr = esp_net_resolve_addr,
        .resolve_start = esp_net_resolve_start,
        .resolve_poll = esp_net_resolve_poll,
        .resolve_close = esp_net_resolve_close,
        .get_host_addr = esp_net_get_host_addr,
        .udp_open = esp_net_udp_open,
        .udp_sendto = esp_net_udp_sendto,
        .udp_recvfrom = esp_net_udp_recvfrom,
        .tcp_open = esp_net_tcp_open,
        .tcp_open_bound = esp_net_tcp_open_bound,
        .tcp_connect = esp_net_tcp_connect,
        .tcp_send = esp_net_tcp_send,
        .tcp_send_timeout = esp_net_tcp_send_timeout,
        .tcp_recv = esp_net_tcp_recv,
        .tls_wrap = esp_net_tls_wrap,
        .icmp_echo = esp_net_icmp_echo,
        .close = esp_net_close,
    };
    static const h2_pal_net_api_t api = {
        .user = NULL,
        .vtable = &vtable,
    };
    return &api;
}
