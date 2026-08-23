#include "fake_net.h"

#include <stdlib.h>
#include <string.h>

static void *fake_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void *fake_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void fake_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static h2_pal_result_t fake_monotonic(void *user, uint64_t *out_ms) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    *out_ms = platform->now_ms;
    platform->now_ms += platform->time_step_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_wall(void *user, uint64_t *out_ms) {
    return fake_monotonic(user, out_ms);
}

static h2_pal_result_t fake_set_wall(void *user, uint64_t wall_ms) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    platform->now_ms = wall_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_wall_status(
    void *user,
    h2_pal_time_wall_status_t *out_status) {
    (void)user;
    out_status->valid = 1u;
    out_status->source = H2_PAL_TIME_WALL_SOURCE_USER;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_sleep(void *user, uint32_t ms) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    platform->now_ms += ms;
    return H2_PAL_OK;
}

static int fake_resolve(
    void *user,
    const char *host,
    h2_pal_net_addr_t *out_addr) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (host == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    platform->resolve_count += 1;
    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->family = H2_PAL_NET_FAMILY_IPV4;
    out_addr->ip[0] = 127u;
    out_addr->ip[3] = 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_resolve_start(
    void *user,
    const char *host,
    h2_pal_net_resolver_t **out_resolver) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (host == NULL || out_resolver == NULL || platform->resolver_active) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    platform->resolve_count += 1;
    platform->resolver_active = 1;
    *out_resolver = (h2_pal_net_resolver_t *)platform;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_resolve_poll(
    void *user,
    h2_pal_net_resolver_t *resolver,
    h2_pal_net_addr_t *out_addr,
    uint32_t timeout_ms) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (resolver != (h2_pal_net_resolver_t *)platform || out_addr == NULL ||
        !platform->resolver_active) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    platform->resolve_poll_count += 1;
    if (platform->resolve_timeout_count > 0) {
        platform->resolve_timeout_count -= 1;
        platform->now_ms += timeout_ms;
        return timeout_ms == 0u
            ? H2_PAL_ERR_WOULD_BLOCK
            : H2_PAL_ERR_TIMEOUT;
    }
    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->family = H2_PAL_NET_FAMILY_IPV4;
    out_addr->ip[0] = 127u;
    out_addr->ip[3] = 1u;
    return H2_PAL_OK;
}

static void fake_resolve_close(
    void *user,
    h2_pal_net_resolver_t *resolver) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (resolver == (h2_pal_net_resolver_t *)platform &&
        platform->resolver_active) {
        platform->resolver_active = 0;
        platform->resolver_close_count += 1;
    }
}

static int fake_host_addr(
    void *user,
    const char *interface_name,
    h2_pal_net_addr_t *out_addr) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (interface_name == NULL || interface_name[0] == '\0' ||
        out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    platform->host_addr_count += 1;
    if (platform->host_addr_result != H2_PAL_OK) {
        return platform->host_addr_result;
    }
    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->family = H2_PAL_NET_FAMILY_IPV4;
    out_addr->ip[0] = 10u;
    out_addr->ip[3] = 2u;
    return H2_PAL_OK;
}

static int fake_tcp_open(
    void *user,
    h2_pal_net_family_t family,
    h2_pal_net_socket_t *out_socket) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (family != H2_PAL_NET_FAMILY_IPV4 || out_socket == NULL ||
        platform->response_index >= platform->response_count) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    platform->open_count += 1;
    platform->response_offset = 0u;
    *out_socket = (int)platform->response_index + 3;
    return H2_PAL_OK;
}

static int fake_tcp_open_bound(
    void *user,
    h2_pal_net_family_t family,
    const h2_pal_net_bind_t *bind,
    h2_pal_net_socket_t *out_socket) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (bind == NULL || bind->type != H2_PAL_NET_BIND_SOURCE_ADDR) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    platform->bound_open_count += 1;
    return fake_tcp_open(user, family, out_socket);
}

static h2_pal_result_t fake_connect(
    void *user,
    h2_pal_net_socket_t socket,
    const h2_pal_net_addr_t *addr,
    uint32_t timeout_ms) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (socket < 0 || addr == NULL || addr->port == 0u || timeout_ms == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    platform->connect_count += 1;
    return H2_PAL_OK;
}

static int fake_send_timeout(
    void *user,
    h2_pal_net_socket_t socket,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (socket < 0 || (data == NULL && len != 0u) || timeout_ms == 0u ||
        len > FAKE_HTTP_REQUEST_BYTES - platform->request_len) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t amount = len;
    if (platform->send_fragment > 0u && amount > platform->send_fragment) {
        amount = platform->send_fragment;
    }
    memcpy(platform->request_bytes + platform->request_len, data, amount);
    platform->request_len += amount;
    return (int)amount;
}

static int fake_send(
    void *user,
    h2_pal_net_socket_t socket,
    const uint8_t *data,
    size_t len) {
    return fake_send_timeout(user, socket, data, len, 1u);
}

static int fake_recv(
    void *user,
    h2_pal_net_socket_t socket,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (socket < 0 || data == NULL || len == 0u || timeout_ms == 0u ||
        platform->response_index >= platform->response_count) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    platform->recv_count += 1;
    if (platform->recv_would_block_count > 0u) {
        platform->recv_would_block_count -= 1u;
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    fake_http_response_t *response =
        &platform->responses[platform->response_index];
    if (platform->response_offset == response->len) {
        return H2_PAL_ERR_CLOSED;
    }
    size_t available = response->len - platform->response_offset;
    size_t amount = len < available ? len : available;
    if (platform->recv_fragment > 0u && amount > platform->recv_fragment) {
        amount = platform->recv_fragment;
    }
    memcpy(data, response->data + platform->response_offset, amount);
    platform->response_offset += amount;
    return (int)amount;
}

static h2_pal_result_t fake_tls_wrap(
    void *user,
    h2_pal_net_socket_t socket,
    const h2_pal_net_tls_config_t *config,
    uint32_t timeout_ms,
    h2_pal_net_socket_t *out_socket) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (socket < 0 || config == NULL || timeout_ms == 0u ||
        out_socket == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    platform->tls_wrap_count += 1;
    platform->tls_config = *config;
    if (config->server_name != NULL) {
        size_t len = strlen(config->server_name);
        if (len >= sizeof(platform->tls_server_name)) {
            return H2_PAL_ERR_NO_SPACE;
        }
        memcpy(platform->tls_server_name, config->server_name, len + 1u);
        platform->tls_config.server_name = platform->tls_server_name;
    }
    if (config->alpn != NULL) {
        size_t len = strlen(config->alpn);
        if (len >= sizeof(platform->tls_alpn)) {
            return H2_PAL_ERR_NO_SPACE;
        }
        memcpy(platform->tls_alpn, config->alpn, len + 1u);
        platform->tls_config.alpn = platform->tls_alpn;
    }
    if (config->root_ca_pem_len > sizeof(platform->tls_root_ca)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    if (config->root_ca_pem_len > 0u) {
        memcpy(platform->tls_root_ca, config->root_ca_pem,
               config->root_ca_pem_len);
        platform->tls_config.root_ca_pem = platform->tls_root_ca;
    }
    *out_socket = socket;
    return H2_PAL_OK;
}

static void fake_close(void *user, h2_pal_net_socket_t socket) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    if (socket >= 0) {
        platform->close_count += 1;
        platform->response_index += 1u;
    }
}

void fake_http_platform_init(fake_http_platform_t *platform) {
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = fake_alloc,
        .realloc = fake_realloc,
        .free = fake_free,
    };
    static const h2_pal_time_vtable_t time_vtable = {
        .get_monotonic_ms = fake_monotonic,
        .get_wall_ms = fake_wall,
        .set_wall_ms = fake_set_wall,
        .get_wall_status = fake_wall_status,
        .sleep_ms = fake_sleep,
    };
    static const h2_pal_net_vtable_t net_vtable = {
        .resolve_addr = fake_resolve,
        .resolve_start = fake_resolve_start,
        .resolve_poll = fake_resolve_poll,
        .resolve_close = fake_resolve_close,
        .get_host_addr = fake_host_addr,
        .tcp_open = fake_tcp_open,
        .tcp_open_bound = fake_tcp_open_bound,
        .tcp_connect = fake_connect,
        .tcp_send = fake_send,
        .tcp_send_timeout = fake_send_timeout,
        .tcp_recv = fake_recv,
        .tls_wrap = fake_tls_wrap,
        .close = fake_close,
    };
    memset(platform, 0, sizeof(*platform));
    platform->mem.user = platform;
    platform->mem.vtable = &mem_vtable;
    platform->time.user = platform;
    platform->time.vtable = &time_vtable;
    platform->net.user = platform;
    platform->net.vtable = &net_vtable;
    platform->recv_fragment = SIZE_MAX;
    platform->send_fragment = SIZE_MAX;
    platform->time_step_ms = 1u;
}

void fake_http_platform_add_response(
    fake_http_platform_t *platform,
    const char *response) {
    if (platform == NULL || response == NULL ||
        platform->response_count >= FAKE_HTTP_MAX_RESPONSES) {
        return;
    }
    platform->responses[platform->response_count].data =
        (const uint8_t *)response;
    platform->responses[platform->response_count].len = strlen(response);
    platform->response_count += 1u;
}
