#include "h2_corehttp_internal.h"

#include <limits.h>
#include <string.h>

static const char *method_name(h2_pal_http_method_t method, size_t *out_len) {
    const char *name = NULL;
    switch (method) {
        case H2_PAL_HTTP_GET:
            name = "GET";
            break;
        case H2_PAL_HTTP_POST:
            name = "POST";
            break;
        case H2_PAL_HTTP_PUT:
            name = "PUT";
            break;
        case H2_PAL_HTTP_PATCH:
            name = "PATCH";
            break;
        case H2_PAL_HTTP_DELETE:
            name = "DELETE";
            break;
        case H2_PAL_HTTP_HEAD:
            name = "HEAD";
            break;
        case H2_PAL_HTTP_OPTIONS:
            name = "OPTIONS";
            break;
        default:
            break;
    }
    if (name != NULL && out_len != NULL) {
        *out_len = strlen(name);
    }
    return name;
}

static uint32_t corehttp_zero_time_ms(void) {
    return 0u;
}

static bool ascii_span_equal_ci(
    const char *data,
    size_t len,
    const char *literal) {
    size_t literal_len = strlen(literal);
    if (data == NULL || len != literal_len) {
        return false;
    }
    for (size_t index = 0u; index < len; ++index) {
        char value = data[index];
        if (value >= 'A' && value <= 'Z') {
            value = (char)(value - 'A' + 'a');
        }
        if (value != literal[index]) {
            return false;
        }
    }
    return true;
}

static h2_pal_result_t validate_span(
    const char *data,
    size_t len,
    bool reject_colon) {
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t index = 0u; index < len; ++index) {
        unsigned char value = (unsigned char)data[index];
        if (value == 0u || value == '\r' || value == '\n' ||
            (reject_colon && value == ':')) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    return H2_PAL_OK;
}

static bool header_name_char_valid(unsigned char value) {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '!' || value == '#' || value == '$' || value == '%' ||
           value == '&' || value == '\'' || value == '*' || value == '+' ||
           value == '-' || value == '.' || value == '^' || value == '_' ||
           value == '`' || value == '|' || value == '~';
}

static h2_pal_result_t validate_header(
    const h2_pal_http_header_t *header) {
    if (header->name.data == NULL || header->name.len == 0u ||
        (header->value.data == NULL && header->value.len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t index = 0u; index < header->name.len; ++index) {
        if (!header_name_char_valid((unsigned char)header->name.data[index])) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    for (size_t index = 0u; index < header->value.len; ++index) {
        unsigned char value = (unsigned char)header->value.data[index];
        if ((value < 0x20u && value != '\t') || value == 0x7fu) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t validate_request(
    const h2_pal_http_request_t *request) {
    size_t unused = 0u;
    if (request == NULL || request->url.data == NULL ||
        request->url.len == 0u || method_name(request->method, &unused) == NULL ||
        (request->header_count > 0u && request->headers == NULL) ||
        (request->body_len > 0u && request->body == NULL) ||
        request->body_len > (size_t)INT32_MAX ||
        (request->chunk_buf_cap > 0u && request->chunk_buf == NULL) ||
        (request->response_buf_cap > 0u && request->response_buf == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = validate_span(
        request->url.data, request->url.len, false);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (size_t index = 0u; index < request->url.len; ++index) {
        unsigned char value = (unsigned char)request->url.data[index];
        if (value <= 0x20u || value == 0x7fu) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    for (size_t index = 0u; index < request->header_count; ++index) {
        const h2_pal_http_header_t *header = &request->headers[index];
        rc = validate_header(header);
        if (rc != H2_PAL_OK ||
            ascii_span_equal_ci(header->name.data, header->name.len, "host") ||
            ascii_span_equal_ci(header->name.data, header->name.len,
                                "content-length") ||
            ascii_span_equal_ci(header->name.data, header->name.len,
                                "transfer-encoding")) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t remaining_ms(
    h2_corehttp_exchange_t *exchange,
    bool slice,
    uint32_t *out_ms) {
    if (h2_pal_http_request_is_canceled(exchange->request)) {
        return H2_PAL_ERR_CLOSED;
    }
    uint64_t now_ms = 0u;
    h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(
        exchange->provider->config.time, &now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (h2_pal_time_deadline_expired(now_ms, exchange->deadline_ms)) {
        return H2_PAL_ERR_TIMEOUT;
    }
    uint64_t remaining = exchange->deadline_ms - now_ms;
    if (remaining > UINT32_MAX) {
        remaining = UINT32_MAX;
    }
    uint32_t value = (uint32_t)remaining;
    if (slice && value > exchange->provider->config.io_slice_ms) {
        value = exchange->provider->config.io_slice_ms;
    }
    if (value == 0u) {
        return H2_PAL_ERR_TIMEOUT;
    }
    *out_ms = value;
    return H2_PAL_OK;
}

static h2_pal_result_t open_transport(
    h2_corehttp_exchange_t *exchange,
    const h2_corehttp_url_t *url) {
    h2_pal_net_bind_t bind;
    h2_pal_net_bind_t *bind_ptr = NULL;
    memset(&bind, 0, sizeof(bind));
    if (exchange->request->interface_name != NULL &&
        exchange->request->interface_name[0] != '\0') {
        bind.type = H2_PAL_NET_BIND_SOURCE_ADDR;
        h2_pal_result_t bind_rc = h2_pal_net_get_host_addr(
            exchange->provider->config.net,
            exchange->request->interface_name,
            &bind.source_addr);
        if (bind_rc != H2_PAL_OK) {
            return bind_rc;
        }
        bind.source_addr.port = 0u;
        bind_ptr = &bind;
    }

    h2_pal_net_addr_t remote;
    memset(&remote, 0, sizeof(remote));
    h2_pal_net_resolver_t *resolver = NULL;
    h2_pal_result_t rc = h2_pal_net_resolve_start(
        exchange->provider->config.net, url->host, &resolver);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (;;) {
        uint32_t timeout_ms = 0u;
        rc = remaining_ms(exchange, true, &timeout_ms);
        if (rc != H2_PAL_OK) {
            break;
        }
        rc = h2_pal_net_resolve_poll(
            exchange->provider->config.net, resolver, &remote, timeout_ms);
        if (rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK) {
            break;
        }
    }
    h2_pal_net_resolve_close(exchange->provider->config.net, resolver);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    remote.port = url->port;

    if (bind_ptr != NULL) {
        if (bind.source_addr.family != remote.family) {
            return H2_PAL_ERR_UNAVAILABLE;
        }
    }
    rc = h2_pal_net_tcp_open_bound(
        exchange->provider->config.net, remote.family, bind_ptr,
        &exchange->socket);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    for (;;) {
        uint32_t timeout_ms = 0u;
        rc = remaining_ms(exchange, true, &timeout_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = h2_pal_net_tcp_connect(
            exchange->provider->config.net, exchange->socket, &remote,
            timeout_ms);
        if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
            continue;
        }
        break;
    }
    if (rc != H2_PAL_OK || !url->secure) {
        return rc;
    }

    uint32_t tls_timeout_ms = 0u;
    rc = remaining_ms(exchange, false, &tls_timeout_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_pal_net_tls_config_t tls = {
        .server_name = url->host,
        .alpn = "http/1.1",
        .root_ca_pem = exchange->provider->config.root_ca_pem,
        .root_ca_pem_len = exchange->provider->config.root_ca_pem_len,
        .verify = exchange->provider->config.tls_verify,
    };
    h2_pal_net_socket_t tls_socket = -1;
    rc = h2_pal_net_tls_wrap(
        exchange->provider->config.net, exchange->socket, &tls,
        tls_timeout_ms, &tls_socket);
    if (rc == H2_PAL_OK) {
        exchange->socket = tls_socket;
    }
    return rc;
}

static h2_pal_result_t map_corehttp_status(
    h2_corehttp_exchange_t *exchange,
    HTTPStatus_t status) {
    switch (status) {
        case HTTPSuccess:
            return H2_PAL_OK;
        case HTTPInvalidParameter:
            return H2_PAL_ERR_INVALID_ARG;
        case HTTPInsufficientMemory:
            return H2_PAL_ERR_NO_SPACE;
        case HTTPNetworkError:
            return exchange->result == H2_PAL_OK ? H2_PAL_ERR_IO
                                                 : exchange->result;
        default:
            return H2_PAL_ERR_FORMAT;
    }
}

static h2_pal_result_t add_request_headers(
    h2_corehttp_exchange_t *exchange,
    HTTPRequestHeaders_t *headers,
    bool forward_authorization) {
    HTTPStatus_t status = HTTPClient_AddHeader(
        headers, "Connection", sizeof("Connection") - 1u, "close",
        sizeof("close") - 1u);
    if (status != HTTPSuccess) {
        return map_corehttp_status(exchange, status);
    }
    for (size_t index = 0u; index < exchange->request->header_count; ++index) {
        const h2_pal_http_header_t *header =
            &exchange->request->headers[index];
        if (!forward_authorization &&
            ascii_span_equal_ci(header->name.data, header->name.len,
                                "authorization")) {
            continue;
        }
        status = HTTPClient_AddHeader(
            headers, header->name.data, header->name.len, header->value.data,
            header->value.len);
        if (status != HTTPSuccess) {
            return map_corehttp_status(exchange, status);
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t send_request(
    h2_corehttp_exchange_t *exchange,
    const h2_corehttp_url_t *url,
    bool forward_authorization) {
    const h2_pal_mem_api_t *allocator =
        exchange->provider->config.allocator;
    uint8_t *buffer = (uint8_t *)h2_pal_mem_alloc(
        allocator, exchange->provider->config.max_header_bytes);
    if (buffer == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    HTTPRequestHeaders_t headers = {
        .pBuffer = buffer,
        .bufferLen = exchange->provider->config.max_header_bytes,
        .headersLen = 0u,
    };
    size_t method_len = 0u;
    const char *method = method_name(exchange->method, &method_len);
    HTTPRequestInfo_t info = {
        .pMethod = method,
        .methodLen = method_len,
        .pPath = url->path,
        .pathLen = url->path_len,
        .pHost = url->authority,
        .hostLen = url->authority_len,
        .reqFlags = 0u,
    };
    HTTPStatus_t status = HTTPClient_InitializeRequestHeaders(&headers, &info);
    h2_pal_result_t rc = map_corehttp_status(exchange, status);
    if (rc == H2_PAL_OK) {
        rc = add_request_headers(exchange, &headers, forward_authorization);
    }
    if (rc == H2_PAL_OK) {
        status = HTTPClient_SendHttpHeaders(
            &exchange->transport, corehttp_zero_time_ms, &headers,
            exchange->body_len, 0u);
        rc = map_corehttp_status(exchange, status);
    }
    h2_pal_mem_free(allocator, buffer);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    size_t offset = 0u;
    while (offset < exchange->body_len) {
        size_t chunk_len = exchange->body_len - offset;
        if (chunk_len > (size_t)INT32_MAX) {
            chunk_len = (size_t)INT32_MAX;
        }
        status = HTTPClient_SendHttpData(
            &exchange->transport, corehttp_zero_time_ms,
            exchange->body + offset, chunk_len);
        rc = map_corehttp_status(exchange, status);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        offset += chunk_len;
    }
    return H2_PAL_OK;
}

static bool retryable_result(h2_pal_result_t rc) {
    return rc == H2_PAL_ERR_IO || rc == H2_PAL_ERR_TIMEOUT ||
           rc == H2_PAL_ERR_CLOSED || rc == H2_PAL_ERR_WOULD_BLOCK ||
           rc == H2_PAL_ERR_UNAVAILABLE;
}

static bool retryable_status(int status_code) {
    return status_code == 408 || status_code == 429 || status_code >= 500;
}

static bool redirect_status(int status_code) {
    return status_code == 301 || status_code == 302 || status_code == 303 ||
           status_code == 307 || status_code == 308;
}

static void free_response_body(h2_pal_http_response_t *response) {
    if (response != NULL && response->allocator != NULL &&
        response->body != NULL) {
        h2_pal_mem_free(response->allocator, response->body);
    }
    h2_pal_http_response_reset(response);
}

static h2_pal_result_t perform_attempt(
    h2_corehttp_t *provider,
    const h2_pal_http_request_t *request,
    h2_pal_http_response_t *response,
    const char *url_data,
    size_t url_len,
    h2_pal_http_method_t method,
    const uint8_t *body,
    size_t body_len,
    uint64_t deadline_ms,
    bool retry_available,
    bool redirect_available,
    bool forward_authorization,
    char **out_redirect,
    size_t *out_redirect_len,
    bool *out_cross_origin,
    bool *out_downgrade,
    bool *out_body_delivered) {
    *out_redirect = NULL;
    *out_redirect_len = 0u;
    *out_cross_origin = false;
    *out_downgrade = false;
    *out_body_delivered = false;
    if (h2_pal_http_request_is_canceled(request)) {
        return H2_PAL_ERR_CLOSED;
    }
    h2_corehttp_url_t url;
    h2_pal_result_t rc = h2_corehttp_parse_url(
        provider, url_data, url_len, &url);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    h2_corehttp_exchange_t exchange;
    memset(&exchange, 0, sizeof(exchange));
    exchange.provider = provider;
    exchange.request = request;
    exchange.response = response;
    exchange.method = method;
    exchange.body = body;
    exchange.body_len = body_len;
    exchange.deadline_ms = deadline_ms;
    exchange.socket = -1;
    exchange.network.exchange = &exchange;
    exchange.transport.pNetworkContext = &exchange.network;
    exchange.transport.send = h2_corehttp_transport_send;
    exchange.transport.recv = h2_corehttp_transport_recv;
    exchange.result = H2_PAL_OK;
    exchange.retry_available = retry_available;
    exchange.redirect_available = redirect_available;

    rc = open_transport(&exchange, &url);
    if (rc == H2_PAL_OK) {
        rc = send_request(&exchange, &url, forward_authorization);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_corehttp_receive_response(&exchange);
    }
    if (exchange.socket >= 0) {
        h2_pal_net_close(provider->config.net, exchange.socket);
        exchange.socket = -1;
    }

    if (rc == H2_PAL_OK && redirect_status(response->status_code) &&
        redirect_available) {
        if (!exchange.location_seen || exchange.location_len == 0u) {
            rc = H2_PAL_ERR_FORMAT;
        } else {
            rc = h2_corehttp_resolve_redirect(
                provider, &url, exchange.location, exchange.location_len,
                out_redirect, out_redirect_len);
            if (rc == H2_PAL_OK) {
                h2_corehttp_url_t redirect_url;
                rc = h2_corehttp_parse_url(
                    provider, *out_redirect, *out_redirect_len, &redirect_url);
                if (rc == H2_PAL_OK) {
                    *out_downgrade = url.secure && !redirect_url.secure;
                    *out_cross_origin =
                        url.secure != redirect_url.secure ||
                        url.port != redirect_url.port ||
                        !ascii_span_equal_ci(
                            url.host, url.host_len, redirect_url.host);
                    h2_corehttp_url_deinit(provider, &redirect_url);
                }
            }
        }
    }
    *out_body_delivered = exchange.body_delivered;
    h2_pal_mem_free(provider->config.allocator, exchange.header_name);
    h2_pal_mem_free(provider->config.allocator, exchange.header_value);
    h2_pal_mem_free(provider->config.allocator, exchange.location);
    h2_corehttp_url_deinit(provider, &url);
    return rc;
}

static int corehttp_request(
    void *user,
    const h2_pal_http_request_t *request,
    h2_pal_http_response_t *out_response) {
    h2_corehttp_t *provider = (h2_corehttp_t *)user;
    h2_pal_result_t rc = validate_request(request);
    if (provider == NULL || out_response == NULL || rc != H2_PAL_OK) {
        return provider == NULL || out_response == NULL
                   ? H2_PAL_ERR_INVALID_ARG
                   : rc;
    }
    h2_pal_http_response_reset(out_response);
    uint64_t start_ms = 0u;
    rc = h2_pal_time_get_monotonic_ms(provider->config.time, &start_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    uint32_t timeout_ms = request->timeout_ms > 0
                              ? (uint32_t)request->timeout_ms
                              : provider->config.default_timeout_ms;
    uint64_t deadline_ms = h2_pal_time_deadline_ms(start_ms, timeout_ms);

    const char *url_data = request->url.data;
    size_t url_len = request->url.len;
    char *owned_url = NULL;
    h2_pal_http_method_t method = request->method;
    const uint8_t *body = request->body;
    size_t body_len = request->body_len;
    uint32_t retries_left = request->retry_count > 0
                                ? (uint32_t)request->retry_count
                                : 0u;
    uint32_t redirects = 0u;
    bool forward_authorization = true;

    for (;;) {
        char *redirect = NULL;
        size_t redirect_len = 0u;
        bool cross_origin = false;
        bool downgrade = false;
        bool body_delivered = false;
        rc = perform_attempt(
            provider, request, out_response, url_data, url_len, method, body,
            body_len, deadline_ms, retries_left > 0u,
            redirects < provider->config.max_redirects,
            forward_authorization, &redirect, &redirect_len, &cross_origin,
            &downgrade, &body_delivered);
        if (rc != H2_PAL_OK) {
            h2_pal_mem_free(provider->config.allocator, redirect);
            if (retries_left > 0u && !body_delivered &&
                !h2_pal_http_request_is_canceled(request) &&
                retryable_result(rc)) {
                retries_left -= 1u;
                free_response_body(out_response);
                continue;
            }
            free_response_body(out_response);
            h2_pal_mem_free(provider->config.allocator, owned_url);
            return rc;
        }

        if (redirect != NULL) {
            if (downgrade) {
                h2_pal_mem_free(provider->config.allocator, redirect);
                free_response_body(out_response);
                h2_pal_mem_free(provider->config.allocator, owned_url);
                return H2_PAL_ERR_TLS_VERIFY;
            }
            redirects += 1u;
            if (cross_origin) {
                forward_authorization = false;
            }
            if (out_response->status_code == 303 ||
                ((out_response->status_code == 301 ||
                  out_response->status_code == 302) &&
                 method == H2_PAL_HTTP_POST)) {
                method = H2_PAL_HTTP_GET;
                body = NULL;
                body_len = 0u;
            }
            free_response_body(out_response);
            h2_pal_mem_free(provider->config.allocator, owned_url);
            owned_url = redirect;
            url_data = owned_url;
            url_len = redirect_len;
            continue;
        }

        if (retryable_status(out_response->status_code) &&
            retries_left > 0u && !body_delivered) {
            retries_left -= 1u;
            free_response_body(out_response);
            continue;
        }
        h2_pal_mem_free(provider->config.allocator, owned_url);
        return H2_PAL_OK;
    }
}

static void corehttp_response_free(
    void *user,
    h2_pal_http_response_t *response) {
    (void)user;
    free_response_body(response);
}

static const h2_pal_http_vtable_t s_corehttp_vtable = {
    .request = corehttp_request,
    .response_free = corehttp_response_free,
};

h2_pal_result_t h2_corehttp_create(
    const h2_corehttp_config_t *config,
    h2_corehttp_t **out_http,
    h2_pal_http_api_t *out_api) {
    if (out_http == NULL || out_api == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_http = NULL;
    memset(out_api, 0, sizeof(*out_api));
    if (config == NULL || config->allocator == NULL || config->net == NULL ||
        config->time == NULL ||
        ((config->root_ca_pem == NULL) != (config->root_ca_pem_len == 0u)) ||
        (uint32_t)config->tls_verify >
            (uint32_t)H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_corehttp_t *provider = (h2_corehttp_t *)h2_pal_mem_alloc(
        config->allocator, sizeof(*provider));
    if (provider == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(provider, 0, sizeof(*provider));
    provider->config = *config;
    if (provider->config.max_header_bytes == 0u) {
        provider->config.max_header_bytes = H2_COREHTTP_DEFAULT_HEADER_BYTES;
    }
    if (provider->config.max_redirects == 0u) {
        provider->config.max_redirects = H2_COREHTTP_DEFAULT_REDIRECTS;
    }
    if (provider->config.default_timeout_ms == 0u) {
        provider->config.default_timeout_ms = H2_COREHTTP_DEFAULT_TIMEOUT_MS;
    }
    if (provider->config.io_slice_ms == 0u) {
        provider->config.io_slice_ms = H2_COREHTTP_DEFAULT_IO_SLICE_MS;
    }
    if (provider->config.tls_verify == H2_PAL_NET_TLS_VERIFY_DEFAULT) {
        provider->config.tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED;
    }
    if (config->root_ca_pem_len > 0u) {
        provider->root_ca_pem = (uint8_t *)h2_pal_mem_alloc(
            config->allocator, config->root_ca_pem_len);
        if (provider->root_ca_pem == NULL) {
            h2_pal_mem_free(config->allocator, provider);
            return H2_PAL_ERR_NO_MEMORY;
        }
        memcpy(provider->root_ca_pem, config->root_ca_pem,
               config->root_ca_pem_len);
        provider->config.root_ca_pem = provider->root_ca_pem;
    }
    provider->api.user = provider;
    provider->api.vtable = &s_corehttp_vtable;
    *out_http = provider;
    *out_api = provider->api;
    return H2_PAL_OK;
}

void h2_corehttp_destroy(h2_corehttp_t *http) {
    if (http == NULL) {
        return;
    }
    const h2_pal_mem_api_t *allocator = http->config.allocator;
    h2_pal_mem_free(allocator, http->root_ca_pem);
    h2_pal_mem_free(allocator, http);
}
