#ifndef H2_COREHTTP_TEST_FAKE_NET_H
#define H2_COREHTTP_TEST_FAKE_NET_H

#include "h2_corehttp.h"

#include <stddef.h>
#include <stdint.h>

#define FAKE_HTTP_MAX_RESPONSES 8u
#define FAKE_HTTP_REQUEST_BYTES 4096u

typedef struct fake_http_response {
    const uint8_t *data;
    size_t len;
} fake_http_response_t;

typedef struct fake_http_platform {
    h2_pal_mem_api_t mem;
    h2_pal_net_api_t net;
    h2_pal_time_api_t time;
    fake_http_response_t responses[FAKE_HTTP_MAX_RESPONSES];
    size_t response_count;
    size_t response_index;
    size_t response_offset;
    size_t recv_fragment;
    size_t send_fragment;
    size_t recv_would_block_count;
    uint8_t request_bytes[FAKE_HTTP_REQUEST_BYTES];
    size_t request_len;
    uint64_t now_ms;
    uint64_t time_step_ms;
    int resolve_count;
    int resolve_poll_count;
    int resolve_timeout_count;
    int resolver_close_count;
    int resolver_active;
    int host_addr_count;
    h2_pal_result_t host_addr_result;
    int open_count;
    int connect_count;
    int tls_wrap_count;
    int close_count;
    int bound_open_count;
    int recv_count;
    h2_pal_net_tls_config_t tls_config;
    uint8_t tls_root_ca[256];
    char tls_server_name[128];
    char tls_alpn[32];
} fake_http_platform_t;

void fake_http_platform_init(fake_http_platform_t *platform);
void fake_http_platform_add_response(
    fake_http_platform_t *platform,
    const char *response);

#endif
