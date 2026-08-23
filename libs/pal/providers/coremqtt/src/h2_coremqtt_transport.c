#include "h2_coremqtt_internal.h"

#include <limits.h>

int32_t h2_coremqtt_transport_recv(NetworkContext_t *network, void *buffer, size_t bytes_to_recv) {
    if (network == NULL || network->client == NULL || buffer == NULL || bytes_to_recv == 0u) {
        return -1;
    }
    h2_pal_mqtt_client_t *client = network->client;
    const h2_pal_net_api_t *net = client->provider->config.net;
    if (bytes_to_recv > (size_t)INT32_MAX) {
        return -1;
    }
    h2_pal_net_socket_t socket = client->tls_socket >= 0 ? client->tls_socket : client->socket;
    int rc = h2_pal_net_tcp_recv(net, socket, buffer, bytes_to_recv, client->recv_timeout_ms);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
        return 0;
    }
    if (rc <= 0) {
        return -1;
    }
    return (int32_t)rc;
}

int32_t h2_coremqtt_transport_send(NetworkContext_t *network, const void *buffer, size_t bytes_to_send) {
    if (network == NULL || network->client == NULL || (buffer == NULL && bytes_to_send != 0u)) {
        return -1;
    }
    h2_pal_mqtt_client_t *client = network->client;
    const h2_pal_net_api_t *net = client->provider->config.net;
    if (bytes_to_send > (size_t)INT32_MAX) {
        return -1;
    }
    h2_pal_net_socket_t socket = client->tls_socket >= 0 ? client->tls_socket : client->socket;
    int rc = h2_pal_net_tcp_send(net, socket, buffer, bytes_to_send);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT) {
        return 0;
    }
    if (rc < 0) {
        return -1;
    }
    return (int32_t)rc;
}
