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

int32_t h2_coremqtt_transport_writev(NetworkContext_t *network, TransportOutVector_t *vectors, size_t count) {
    if (network == NULL || network->client == NULL || vectors == NULL || count == 0u) return -1;
    size_t total = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if ((vectors[i].iov_base == NULL && vectors[i].iov_len != 0u) ||
            vectors[i].iov_len > (size_t)INT32_MAX - total) return -1;
        total += vectors[i].iov_len;
    }
    int32_t sent = 0;
    /* PAL send is synchronous. Submit the complete scatter transaction when
     * each vector makes full progress, so coreMQTT does not discard an unsent
     * tail solely because one successful write exceeded its retry interval.
     * A short write or refusal returns only the actually accepted prefix. The
     * core still owns retry/deadline decisions; no data is buffered or copied. */
    for (size_t i = 0u; i < count; ++i) {
        if (vectors[i].iov_len == 0u) continue;
        int32_t rc = h2_coremqtt_transport_send(network, vectors[i].iov_base, vectors[i].iov_len);
        if (rc <= 0) return sent > 0 ? sent : rc;
        if ((size_t)rc > vectors[i].iov_len) return -1;
        sent += rc;
        if ((size_t)rc < vectors[i].iov_len) return sent;
    }
    return sent;
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
