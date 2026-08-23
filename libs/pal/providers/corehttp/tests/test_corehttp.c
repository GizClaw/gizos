#include "fake_net.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static const uint8_t *find_bytes(
    const uint8_t *haystack,
    size_t haystack_len,
    const char *needle,
    size_t needle_len) {
    if (needle_len == 0u) {
        return haystack;
    }
    if (haystack == NULL || needle == NULL || needle_len > haystack_len) {
        return NULL;
    }
    for (size_t index = 0u; index <= haystack_len - needle_len; ++index) {
        if (memcmp(haystack + index, needle, needle_len) == 0) {
            return haystack + index;
        }
    }
    return NULL;
}

static size_t count_bytes(
    const uint8_t *data,
    size_t data_len,
    const char *needle,
    size_t needle_len) {
    size_t count = 0u;
    size_t offset = 0u;
    while (offset <= data_len) {
        const uint8_t *match = find_bytes(
            data + offset, data_len - offset, needle, needle_len);
        if (match == NULL) {
            break;
        }
        count += 1u;
        offset = (size_t)(match - data) + needle_len;
    }
    return count;
}

static h2_corehttp_t *create_provider(
    fake_http_platform_t *platform,
    h2_pal_http_api_t *out_api,
    const uint8_t *root_ca,
    size_t root_ca_len) {
    h2_corehttp_config_t config = {
        .allocator = &platform->mem,
        .net = &platform->net,
        .time = &platform->time,
        .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
        .root_ca_pem = root_ca,
        .root_ca_pem_len = root_ca_len,
        .max_header_bytes = 4096u,
        .max_redirects = 5u,
        .default_timeout_ms = 1000u,
        .io_slice_ms = 25u,
    };
    h2_corehttp_t *provider = NULL;
    if (h2_corehttp_create(&config, &provider, out_api) != H2_PAL_OK) {
        return NULL;
    }
    return provider;
}

static h2_corehttp_t *create_provider_with_header_limit(
    fake_http_platform_t *platform,
    h2_pal_http_api_t *out_api,
    size_t max_header_bytes) {
    h2_corehttp_config_t config = {
        .allocator = &platform->mem,
        .net = &platform->net,
        .time = &platform->time,
        .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
        .max_header_bytes = max_header_bytes,
        .max_redirects = 5u,
        .default_timeout_ms = 1000u,
        .io_slice_ms = 25u,
    };
    h2_corehttp_t *provider = NULL;
    if (h2_corehttp_create(&config, &provider, out_api) != H2_PAL_OK) {
        return NULL;
    }
    return provider;
}

static int test_create_failure_resets_outputs(void) {
    h2_corehttp_config_t invalid_config = {0};
    h2_corehttp_t *provider = (h2_corehttp_t *)(uintptr_t)1u;
    h2_pal_http_api_t api;
    memset(&api, 0xa5, sizeof(api));

    CHECK(h2_corehttp_create(&invalid_config, &provider, &api) ==
          H2_PAL_ERR_INVALID_ARG);
    CHECK(provider == NULL);
    CHECK(api.user == NULL);
    CHECK(api.vtable == NULL);
    return 0;
}

static int test_content_length_response(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform,
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    uint8_t body[8] = {0};
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/path?q=1", .len = 28u},
        .response_buf = body,
        .response_buf_cap = sizeof(body),
    };
    h2_pal_http_response_t response;
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    CHECK(response.status_code == 200);
    CHECK(response.content_length == 5);
    CHECK(response.body == body);
    CHECK(response.body_len == 5u);
    CHECK(memcmp(response.body, "hello", 5u) == 0);
    CHECK(platform.open_count == 1);
    CHECK(platform.close_count == 1);
    CHECK(find_bytes(platform.request_bytes, platform.request_len,
                     "GET /path?q=1 HTTP/1.1", 22u) != NULL);
    CHECK(find_bytes(platform.request_bytes, platform.request_len,
                     "Host: example.test", 18u) != NULL);
    h2_corehttp_destroy(provider);
    return 0;
}

typedef struct stream_result {
    uint8_t body[16];
    size_t len;
    size_t callbacks;
} stream_result_t;

static int stream_body(
    void *user,
    const h2_pal_http_request_t *request,
    const uint8_t *chunk,
    size_t chunk_len,
    size_t total_read,
    size_t remaining) {
    (void)request;
    stream_result_t *result = (stream_result_t *)user;
    if (chunk_len > sizeof(result->body) - result->len ||
        total_read != result->len + chunk_len || remaining != 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    memcpy(result->body + result->len, chunk, chunk_len);
    result->len += chunk_len;
    result->callbacks += 1u;
    return H2_PAL_OK;
}

static int stream_body_with_length(
    void *user,
    const h2_pal_http_request_t *request,
    const uint8_t *chunk,
    size_t chunk_len,
    size_t total_read,
    size_t remaining) {
    (void)request;
    stream_result_t *result = (stream_result_t *)user;
    if (chunk_len > sizeof(result->body) - result->len ||
        total_read != result->len + chunk_len ||
        remaining != 4u - total_read) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    memcpy(result->body + result->len, chunk, chunk_len);
    result->len += chunk_len;
    result->callbacks += 1u;
    return H2_PAL_OK;
}

typedef struct header_result {
    char value[16];
    size_t value_len;
    size_t callbacks;
    int result;
} header_result_t;

static int capture_response_header(
    void *user,
    const h2_pal_http_request_t *request,
    h2_pal_http_str_t name,
    h2_pal_http_str_t value) {
    (void)request;
    header_result_t *result = (header_result_t *)user;
    result->callbacks += 1u;
    if (name.len == sizeof("X-Total-Count") - 1u &&
        memcmp(name.data, "X-Total-Count", name.len) == 0) {
        if (value.len > sizeof(result->value)) {
            return H2_PAL_ERR_NO_SPACE;
        }
        memcpy(result->value, value.data, value.len);
        result->value_len = value.len;
    }
    return result->result;
}

static int test_response_header_callback(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    platform.recv_fragment = 1u;
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 204 No Content\r\n"
                   "X-Total-Count:\t 42 \t\r\n"
                   "X-Empty:\r\n"
                   "Content-Length: 0\r\n\r\n");
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    header_result_t headers = {.result = H2_PAL_OK};
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
        .response_header_cb = capture_response_header,
        .response_header_user = &headers,
    };
    h2_pal_http_response_t response;
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    CHECK(headers.callbacks == 3u);
    CHECK(headers.value_len == 2u &&
          memcmp(headers.value, "42", headers.value_len) == 0);
    h2_corehttp_destroy(provider);

    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nX-Abort: yes\r\n"
                   "Content-Length: 0\r\n\r\n");
    provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    headers = (header_result_t){.result = H2_PAL_ERR_CLOSED};
    request.response_header_user = &headers;
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_CLOSED);
    CHECK(headers.callbacks == 1u);
    CHECK(platform.open_count == 1 && platform.close_count == 1);
    h2_corehttp_destroy(provider);
    return 0;
}

static int test_fragmented_chunked_stream(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    platform.recv_fragment = 1u;
    fake_http_platform_add_response(
        &platform,
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    uint8_t chunk[2];
    stream_result_t stream = {0};
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
        .chunk_buf = chunk,
        .chunk_buf_cap = sizeof(chunk),
        .read_cb = stream_body,
        .user = &stream,
    };
    h2_pal_http_response_t response;
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    CHECK(response.content_length == -1);
    CHECK(response.body_len == 5u);
    CHECK(stream.len == 5u);
    CHECK(memcmp(stream.body, "abcde", 5u) == 0);
    CHECK(stream.callbacks >= 3u);
    h2_corehttp_destroy(provider);
    return 0;
}

static int test_redirect_and_retry(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform,
        "HTTP/1.1 302 Found\r\nLocation: https://other.test/final\r\n"
        "Content-Length: 0\r\n\r\n");
    fake_http_platform_add_response(
        &platform,
        "HTTP/1.1 503 Busy\r\nContent-Length: 4\r\n\r\nnope");
    fake_http_platform_add_response(
        &platform,
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const uint8_t ca[] = "test-ca";
    h2_pal_http_api_t api;
    h2_corehttp_t *provider =
        create_provider(&platform, &api, ca, sizeof(ca));
    CHECK(provider != NULL);
    h2_pal_http_header_t header = {
        .name = {.data = "Authorization", .len = 13u},
        .value = {.data = "Bearer secret", .len = 13u},
    };
    uint8_t body[4] = {0};
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_POST,
        .url = {.data = "http://example.test/start", .len = 25u},
        .headers = &header,
        .header_count = 1u,
        .body = (const uint8_t *)"data",
        .body_len = 4u,
        .response_buf = body,
        .response_buf_cap = sizeof(body),
        .retry_count = 1,
    };
    h2_pal_http_response_t response;
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    CHECK(response.status_code == 200);
    CHECK(response.body_len == 2u);
    CHECK(memcmp(body, "ok", 2u) == 0);
    CHECK(platform.open_count == 3);
    CHECK(platform.close_count == 3);
    CHECK(platform.tls_wrap_count == 2);
    CHECK(platform.tls_config.verify == H2_PAL_NET_TLS_VERIFY_REQUIRED);
    CHECK(platform.tls_config.root_ca_pem_len == sizeof(ca));
    CHECK(memcmp(platform.tls_config.root_ca_pem, ca, sizeof(ca)) == 0);
    const uint8_t *second = find_bytes(
        platform.request_bytes, platform.request_len,
        "GET /final HTTP/1.1", 19u);
    CHECK(second != NULL);
    CHECK(find_bytes(
              second,
              platform.request_len -
                  (size_t)(second - platform.request_bytes),
              "Authorization", 13u) == NULL);
    h2_corehttp_destroy(provider);
    return 0;
}

static int test_bound_https(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform,
        "HTTP/1.1 204 No Content\r\n\r\n");
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_HEAD,
        .url = {.data = "https://example.test/status", .len = 27u},
        .interface_name = "wifi0",
    };
    h2_pal_http_response_t response;
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    CHECK(response.status_code == 204);
    CHECK(platform.bound_open_count == 1);
    CHECK(platform.host_addr_count == 1);
    CHECK(platform.tls_wrap_count == 1);
    CHECK(strcmp(platform.tls_config.server_name, "example.test") == 0);
    CHECK(strcmp(platform.tls_config.alpn, "http/1.1") == 0);
    h2_corehttp_destroy(provider);

    fake_http_platform_init(&platform);
    platform.host_addr_result = H2_PAL_ERR_UNAVAILABLE;
    provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_UNAVAILABLE);
    CHECK(platform.host_addr_count == 1);
    CHECK(platform.resolve_count == 0);
    CHECK(platform.open_count == 0);
    h2_corehttp_destroy(provider);
    return 0;
}

static int test_rejects_malformed_request(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    h2_pal_http_response_t response;
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_POST,
        .url = {.data = "http://example.test/", .len = 20u},
        .body = (const uint8_t *)"x",
        .body_len = (size_t)INT32_MAX + 1u,
    };
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_INVALID_ARG);
    h2_pal_http_header_t bad_header = {
        .name = {.data = "bad name", .len = 8u},
        .value = {.data = "value", .len = 5u},
    };
    request = (h2_pal_http_request_t){
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
        .headers = &bad_header,
        .header_count = 1u,
    };
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_INVALID_ARG);
    request = (h2_pal_http_request_t){
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/a b", .len = 23u},
    };
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_INVALID_ARG);
    static const char *const reserved_names[] = {
        "Host", "Content-Length", "Transfer-Encoding",
    };
    for (size_t index = 0u;
         index < sizeof(reserved_names) / sizeof(reserved_names[0]); ++index) {
        h2_pal_http_header_t reserved = {
            .name = {.data = reserved_names[index],
                     .len = strlen(reserved_names[index])},
            .value = {.data = "value", .len = 5u},
        };
        request = (h2_pal_http_request_t){
            .method = H2_PAL_HTTP_GET,
            .url = {.data = "http://example.test/", .len = 20u},
            .headers = &reserved,
            .header_count = 1u,
        };
        CHECK(h2_pal_http_request(&api, &request, &response) ==
              H2_PAL_ERR_INVALID_ARG);
    }
    static const char *const bad_urls[] = {
        "ftp://example.test/",
        "http://user@example.test/",
        "http://example.test/path#fragment",
        "http://example.test:0/",
        "http://example.test:42949672961/",
    };
    for (size_t index = 0u;
         index < sizeof(bad_urls) / sizeof(bad_urls[0]); ++index) {
        request = (h2_pal_http_request_t){
            .method = H2_PAL_HTTP_GET,
            .url = {.data = bad_urls[index], .len = strlen(bad_urls[index])},
        };
        CHECK(h2_pal_http_request(&api, &request, &response) != H2_PAL_OK);
    }
    CHECK(platform.open_count == 0);
    h2_corehttp_destroy(provider);
    return 0;
}

static int test_url_forms_and_explicit_ports(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 204 No Content\r\n\r\n");
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 204 No Content\r\n\r\n");
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    h2_pal_http_response_t response;
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://127.0.0.1:8080?q=1",
                .len = sizeof("http://127.0.0.1:8080?q=1") - 1u},
    };
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    request.url = (h2_pal_http_str_t){
        .data = "http://[::1]:8081/ipv6",
        .len = sizeof("http://[::1]:8081/ipv6") - 1u};
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    CHECK(find_bytes(platform.request_bytes, platform.request_len,
                     "GET /?q=1 HTTP/1.1",
                     sizeof("GET /?q=1 HTTP/1.1") - 1u) != NULL);
    CHECK(find_bytes(platform.request_bytes, platform.request_len,
                     "Host: 127.0.0.1:8080",
                     sizeof("Host: 127.0.0.1:8080") - 1u) != NULL);
    CHECK(find_bytes(platform.request_bytes, platform.request_len,
                     "Host: [::1]:8081",
                     sizeof("Host: [::1]:8081") - 1u) != NULL);
    h2_corehttp_destroy(provider);
    return 0;
}

static int test_all_methods_and_partial_send(void) {
    static const h2_pal_http_method_t methods[] = {
        H2_PAL_HTTP_GET, H2_PAL_HTTP_POST, H2_PAL_HTTP_PUT,
        H2_PAL_HTTP_PATCH, H2_PAL_HTTP_DELETE, H2_PAL_HTTP_HEAD,
        H2_PAL_HTTP_OPTIONS,
    };
    static const char *const names[] = {
        "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS",
    };
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    platform.send_fragment = 3u;
    for (size_t index = 0u; index < sizeof(methods) / sizeof(methods[0]);
         ++index) {
        fake_http_platform_add_response(
            &platform, "HTTP/1.1 204 No Content\r\n\r\n");
    }
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    for (size_t index = 0u; index < sizeof(methods) / sizeof(methods[0]);
         ++index) {
        h2_pal_http_request_t request = {
            .method = methods[index],
            .url = {.data = "http://example.test/method", .len = 26u},
        };
        h2_pal_http_response_t response;
        CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
        CHECK(response.status_code == 204);
        char line[40];
        int len = snprintf(line, sizeof(line), "%s /method HTTP/1.1",
                           names[index]);
        CHECK(len > 0 && (size_t)len < sizeof(line));
        CHECK(find_bytes(platform.request_bytes, platform.request_len, line,
                         (size_t)len) != NULL);
    }
    CHECK(platform.open_count == 7);
    CHECK(platform.close_count == 7);
    h2_corehttp_destroy(provider);
    return 0;
}

static int test_body_modes_and_response_framing(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    platform.recv_fragment = 2u;
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 100 Continue\r\n\r\n"
                   "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none");
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\ntwo");
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nfour");
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nbody");
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);

    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
        .response_allocator = &platform.mem,
    };
    h2_pal_http_response_t response;
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    CHECK(response.status_code == 200);
    CHECK(response.body_len == 3u && memcmp(response.body, "one", 3u) == 0);
    h2_pal_http_response_free(&api, &response);
    CHECK(response.body == NULL);

    request.response_allocator = NULL;
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    CHECK(response.content_length == -1);
    CHECK(response.body == NULL && response.body_len == 3u);

    uint8_t too_small[3];
    request.response_buf = too_small;
    request.response_buf_cap = sizeof(too_small);
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_NO_SPACE);
    CHECK(platform.close_count == 3);

    request = (h2_pal_http_request_t){
        .method = H2_PAL_HTTP_HEAD,
        .url = {.data = "http://example.test/", .len = 20u},
    };
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    CHECK(response.status_code == 200 && response.body_len == 0u);
    h2_corehttp_destroy(provider);
    return 0;
}

static int cancel_request(void *user) {
    int *cancel = (int *)user;
    return *cancel;
}

typedef struct cancel_after_recv {
    fake_http_platform_t *platform;
    int recv_count;
} cancel_after_recv_t;

static int cancel_midflight(void *user) {
    cancel_after_recv_t *cancel = (cancel_after_recv_t *)user;
    return cancel->platform->recv_count >= cancel->recv_count;
}

static int cancel_after_resolve_poll(void *user) {
    fake_http_platform_t *platform = (fake_http_platform_t *)user;
    return platform->resolve_poll_count > 0;
}

static int test_dns_deadline_and_cancel(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    platform.resolve_timeout_count = 8;
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
        .timeout_ms = 40,
    };
    h2_pal_http_response_t response;
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_TIMEOUT);
    CHECK(platform.resolve_poll_count >= 1);
    CHECK(platform.resolver_close_count == 1);
    CHECK(platform.open_count == 0);
    h2_corehttp_destroy(provider);

    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    platform.resolve_timeout_count = 8;
    request = (h2_pal_http_request_t){
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
        .cancel_cb = cancel_after_resolve_poll,
        .cancel_user = &platform,
    };
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_CLOSED);
    CHECK(platform.resolve_poll_count == 1);
    CHECK(platform.resolver_close_count == 1);
    CHECK(platform.open_count == 0);
    h2_corehttp_destroy(provider);
    return 0;
}

static int test_cancel_deadline_and_malformed_response(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    int cancel = 1;
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
        .cancel_cb = cancel_request,
        .cancel_user = &cancel,
    };
    h2_pal_http_response_t response;
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_ERR_CLOSED);
    CHECK(platform.resolve_count == 0 && platform.open_count == 0);

    cancel = 0;
    platform.recv_would_block_count = 8u;
    platform.time_step_ms = 10u;
    request.timeout_ms = 25;
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_ERR_TIMEOUT);
    CHECK(platform.close_count == 1);
    h2_corehttp_destroy(provider);

    fake_http_platform_init(&platform);
    platform.recv_fragment = 1u;
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    cancel_after_recv_t midflight = {
        .platform = &platform,
        .recv_count = 1,
    };
    request = (h2_pal_http_request_t){
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
        .cancel_cb = cancel_midflight,
        .cancel_user = &midflight,
        .retry_count = 3,
    };
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_CLOSED);
    CHECK(platform.open_count == 1 && platform.close_count == 1);
    h2_corehttp_destroy(provider);

    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nab");
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone");
    provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    uint8_t chunk[4];
    stream_result_t stream = {0};
    request = (h2_pal_http_request_t){
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
        .chunk_buf = chunk,
        .chunk_buf_cap = sizeof(chunk),
        .read_cb = stream_body_with_length,
        .user = &stream,
        .retry_count = 1,
    };
    CHECK(h2_pal_http_request(&api, &request, &response) != H2_PAL_OK);
    CHECK(stream.len == 2u);
    CHECK(platform.open_count == 1 && platform.close_count == 1);
    h2_corehttp_destroy(provider);

    fake_http_platform_init(&platform);
    fake_http_platform_add_response(&platform, "not-http\r\n\r\n");
    provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    request = (h2_pal_http_request_t){
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
    };
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_ERR_FORMAT);
    CHECK(platform.close_count == 1);
    h2_corehttp_destroy(provider);

    return 0;
}

static int test_retry_status_boundaries(void) {
    static const int statuses[] = {408, 429, 499, 500, 599};
    static const int retries[] = {1, 1, 0, 1, 1};
    for (size_t index = 0u;
         index < sizeof(statuses) / sizeof(statuses[0]); ++index) {
        fake_http_platform_t platform;
        fake_http_platform_init(&platform);
        char first[96];
        int len = snprintf(first, sizeof(first),
                           "HTTP/1.1 %d Test\r\nContent-Length: 0\r\n\r\n",
                           statuses[index]);
        CHECK(len > 0 && (size_t)len < sizeof(first));
        fake_http_platform_add_response(&platform, first);
        if (retries[index] != 0) {
            fake_http_platform_add_response(
                &platform,
                "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        }
        h2_pal_http_api_t api;
        h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
        CHECK(provider != NULL);
        h2_pal_http_request_t request = {
            .method = H2_PAL_HTTP_GET,
            .url = {.data = "http://example.test/", .len = 20u},
            .retry_count = 1,
        };
        h2_pal_http_response_t response;
        CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
        CHECK(response.status_code == (retries[index] ? 200 : statuses[index]));
        CHECK(platform.open_count == 1 + retries[index]);
        CHECK(platform.close_count == platform.open_count);
        h2_corehttp_destroy(provider);
    }
    return 0;
}

static int test_malformed_and_oversized_responses(void) {
    static const char *const malformed[] = {
        "HTTP/1.1 200 OK\r\nBad Header: value\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\nx\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nshorter",
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokextra",
    };
    for (size_t index = 0u;
         index < sizeof(malformed) / sizeof(malformed[0]); ++index) {
        fake_http_platform_t platform;
        fake_http_platform_init(&platform);
        fake_http_platform_add_response(&platform, malformed[index]);
        h2_pal_http_api_t api;
        h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
        CHECK(provider != NULL);
        h2_pal_http_request_t request = {
            .method = H2_PAL_HTTP_GET,
            .url = {.data = "http://example.test/", .len = 20u},
        };
        h2_pal_http_response_t response;
        CHECK(h2_pal_http_request(&api, &request, &response) ==
              H2_PAL_ERR_FORMAT);
        CHECK(platform.close_count == 1);
        h2_corehttp_destroy(provider);
    }

    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform,
        "HTTP/1.1 200 OK\r\nX-Oversized: 0123456789012345678901234567890123456789012345678901234567890123456789\r\nContent-Length: 0\r\n\r\n");
    h2_pal_http_api_t api;
    h2_corehttp_t *provider =
        create_provider_with_header_limit(&platform, &api, 96u);
    CHECK(provider != NULL);
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/", .len = 20u},
    };
    h2_pal_http_response_t response;
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_NO_SPACE);
    CHECK(platform.close_count == 1);
    h2_corehttp_destroy(provider);

    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform,
        "HTTP/1.1 200 "
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "\r\nContent-Length: 0\r\n\r\n");
    provider = create_provider_with_header_limit(&platform, &api, 48u);
    CHECK(provider != NULL);
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_NO_SPACE);
    CHECK(platform.close_count == 1);
    h2_corehttp_destroy(provider);
    return 0;
}

static int test_redirect_preserves_method_and_blocks_downgrade(void) {
    fake_http_platform_t platform;
    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 307 Temporary Redirect\r\n"
                   "Location: /next\r\nContent-Length: 0\r\n\r\n");
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    const uint8_t ca[] = "ca";
    h2_pal_http_api_t api;
    h2_corehttp_t *provider = create_provider(
        &platform, &api, ca, sizeof(ca));
    CHECK(provider != NULL);
    h2_pal_http_request_t request = {
        .method = H2_PAL_HTTP_PUT,
        .url = {.data = "https://example.test/start", .len = 26u},
        .body = (const uint8_t *)"data",
        .body_len = 4u,
    };
    h2_pal_http_response_t response;
    CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
    CHECK(count_bytes(platform.request_bytes, platform.request_len,
                      "PUT ", 4u) == 2u);
    CHECK(count_bytes(platform.request_bytes, platform.request_len,
                      "data", 4u) == 2u);
    h2_corehttp_destroy(provider);

    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 302 Found\r\n"
                   "Location: http://example.test/plain\r\n"
                   "Content-Length: 0\r\n\r\n");
    provider = create_provider(&platform, &api, ca, sizeof(ca));
    CHECK(provider != NULL);
    request = (h2_pal_http_request_t){
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "https://example.test/start", .len = 26u},
    };
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_TLS_VERIFY);
    CHECK(platform.open_count == 1 && platform.close_count == 1);
    h2_corehttp_destroy(provider);

    fake_http_platform_init(&platform);
    fake_http_platform_add_response(
        &platform, "HTTP/1.1 302 Found\r\n"
                   "Location: ftp://other.test/file\r\n"
                   "Content-Length: 0\r\n\r\n");
    provider = create_provider(&platform, &api, NULL, 0u);
    CHECK(provider != NULL);
    request = (h2_pal_http_request_t){
        .method = H2_PAL_HTTP_GET,
        .url = {.data = "http://example.test/start", .len = 25u},
    };
    CHECK(h2_pal_http_request(&api, &request, &response) ==
          H2_PAL_ERR_UNSUPPORTED);
    CHECK(platform.open_count == 1 && platform.close_count == 1);
    h2_corehttp_destroy(provider);
    return 0;
}

static int test_redirect_method_rules(void) {
    typedef struct redirect_case {
        int status;
        h2_pal_http_method_t method;
        const char *initial_method;
        const char *redirect_method;
        size_t body_count;
    } redirect_case_t;
    static const redirect_case_t cases[] = {
        {301, H2_PAL_HTTP_POST, "POST ", "GET ", 1u},
        {303, H2_PAL_HTTP_PUT, "PUT ", "GET ", 1u},
        {308, H2_PAL_HTTP_PUT, "PUT ", "PUT ", 2u},
    };
    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        fake_http_platform_t platform;
        fake_http_platform_init(&platform);
        char redirect[128];
        int len = snprintf(
            redirect, sizeof(redirect),
            "HTTP/1.1 %d Redirect\r\nLocation: /next\r\n"
            "Content-Length: 0\r\n\r\n",
            cases[index].status);
        CHECK(len > 0 && (size_t)len < sizeof(redirect));
        fake_http_platform_add_response(&platform, redirect);
        fake_http_platform_add_response(
            &platform, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        h2_pal_http_api_t api;
        h2_corehttp_t *provider = create_provider(&platform, &api, NULL, 0u);
        CHECK(provider != NULL);
        h2_pal_http_request_t request = {
            .method = cases[index].method,
            .url = {.data = "http://example.test/start", .len = 25u},
            .body = (const uint8_t *)"data",
            .body_len = 4u,
        };
        h2_pal_http_response_t response;
        CHECK(h2_pal_http_request(&api, &request, &response) == H2_PAL_OK);
        CHECK(count_bytes(platform.request_bytes, platform.request_len,
                          cases[index].initial_method,
                          strlen(cases[index].initial_method)) >= 1u);
        CHECK(count_bytes(platform.request_bytes, platform.request_len,
                          cases[index].redirect_method,
                          strlen(cases[index].redirect_method)) >= 1u);
        CHECK(count_bytes(platform.request_bytes, platform.request_len,
                          "data", 4u) == cases[index].body_count);
        CHECK(platform.open_count == 2 && platform.close_count == 2);
        h2_corehttp_destroy(provider);
    }
    return 0;
}

int main(void) {
    int rc = test_create_failure_resets_outputs();
    if (rc == 0) {
        rc = test_content_length_response();
    }
    if (rc == 0) {
        rc = test_fragmented_chunked_stream();
    }
    if (rc == 0) {
        rc = test_response_header_callback();
    }
    if (rc == 0) {
        rc = test_redirect_and_retry();
    }
    if (rc == 0) {
        rc = test_bound_https();
    }
    if (rc == 0) {
        rc = test_rejects_malformed_request();
    }
    if (rc == 0) {
        rc = test_url_forms_and_explicit_ports();
    }
    if (rc == 0) {
        rc = test_all_methods_and_partial_send();
    }
    if (rc == 0) {
        rc = test_body_modes_and_response_framing();
    }
    if (rc == 0) {
        rc = test_cancel_deadline_and_malformed_response();
    }
    if (rc == 0) {
        rc = test_dns_deadline_and_cancel();
    }
    if (rc == 0) {
        rc = test_retry_status_boundaries();
    }
    if (rc == 0) {
        rc = test_malformed_and_oversized_responses();
    }
    if (rc == 0) {
        rc = test_redirect_preserves_method_and_blocks_downgrade();
    }
    if (rc == 0) {
        rc = test_redirect_method_rules();
    }
    return rc;
}
