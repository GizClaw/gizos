#ifndef H2_COREHTTP_INTERNAL_H
#define H2_COREHTTP_INTERNAL_H

#include "h2_corehttp.h"

#include "core_http_client.h"
#include "llhttp.h"

#include <stdbool.h>

#define H2_COREHTTP_DEFAULT_HEADER_BYTES 16384u
#define H2_COREHTTP_DEFAULT_REDIRECTS 5u
#define H2_COREHTTP_DEFAULT_TIMEOUT_MS 10000u
#define H2_COREHTTP_DEFAULT_IO_SLICE_MS 100u
#define H2_COREHTTP_RECV_BYTES 2048u

typedef struct h2_corehttp_url {
    char *host;
    size_t host_len;
    char *authority;
    size_t authority_len;
    char *path;
    size_t path_len;
    uint16_t port;
    bool secure;
} h2_corehttp_url_t;

struct h2_corehttp {
    h2_corehttp_config_t config;
    h2_pal_http_api_t api;
    uint8_t *root_ca_pem;
};

typedef struct h2_corehttp_exchange h2_corehttp_exchange_t;

struct NetworkContext {
    h2_corehttp_exchange_t *exchange;
};

struct h2_corehttp_exchange {
    h2_corehttp_t *provider;
    const h2_pal_http_request_t *request;
    h2_pal_http_response_t *response;
    h2_pal_http_method_t method;
    const uint8_t *body;
    size_t body_len;
    uint64_t deadline_ms;
    h2_pal_net_socket_t socket;
    struct NetworkContext network;
    TransportInterface_t transport;
    h2_pal_result_t result;
    llhttp_t parser;
    llhttp_settings_t parser_settings;
    size_t header_bytes;
    char *header_name;
    size_t header_name_len;
    size_t header_name_cap;
    char *header_value;
    size_t header_value_len;
    size_t header_value_cap;
    char *location;
    size_t location_len;
    size_t location_cap;
    bool capture_location;
    bool location_seen;
    bool headers_complete;
    bool message_complete;
    bool suppress_body;
    bool retry_available;
    bool redirect_available;
    bool body_delivered;
    size_t total_read;
};

h2_pal_result_t h2_corehttp_parse_url(
    h2_corehttp_t *provider,
    const char *data,
    size_t len,
    h2_corehttp_url_t *out_url);
void h2_corehttp_url_deinit(h2_corehttp_t *provider, h2_corehttp_url_t *url);
h2_pal_result_t h2_corehttp_resolve_redirect(
    h2_corehttp_t *provider,
    const h2_corehttp_url_t *base,
    const char *location,
    size_t location_len,
    char **out_url,
    size_t *out_url_len);

int32_t h2_corehttp_transport_recv(
    NetworkContext_t *network,
    void *buffer,
    size_t bytes_to_recv);
int32_t h2_corehttp_transport_send(
    NetworkContext_t *network,
    const void *buffer,
    size_t bytes_to_send);

h2_pal_result_t h2_corehttp_receive_response(h2_corehttp_exchange_t *exchange);

#endif
