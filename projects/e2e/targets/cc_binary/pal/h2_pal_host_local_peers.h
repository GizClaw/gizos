#ifndef H2_PAL_HOST_LOCAL_PEERS_H
#define H2_PAL_HOST_LOCAL_PEERS_H

#include "h2_pal_host_fixture.h"

typedef struct h2_pal_host_local_peers h2_pal_host_local_peers_t;

typedef struct h2_pal_host_local_peer_endpoints {
    uint16_t tcp_echo_port;
    uint16_t tls_echo_port;
    uint16_t tls_wrong_ca_port;
    uint16_t https_port;
    uint16_t mqtt_port;
    const uint8_t *root_ca_pem;
    size_t root_ca_pem_len;
    const uint8_t *wrong_ca_pem;
    size_t wrong_ca_pem_len;
} h2_pal_host_local_peer_endpoints_t;

h2_pal_result_t h2_pal_host_local_peers_create(
    const h2_pal_host_fixture_config_t *config,
    h2_pal_host_local_peers_t **out_peers,
    h2_pal_host_local_peer_endpoints_t *out_endpoints);

h2_pal_result_t h2_pal_host_local_peers_destroy(
    h2_pal_host_local_peers_t *peers);

#endif
