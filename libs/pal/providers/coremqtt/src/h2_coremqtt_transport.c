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

static int transaction_start(h2_pal_mqtt_client_t *client, uint64_t *deadline) {
    if (client->send_result != H2_PAL_OK) return 0;
    uint64_t current = 0u;
    int rc = h2_pal_time_get_monotonic_ms(client->time_api, &current);
    if (rc != H2_PAL_OK) {client->send_result = rc;return 0;}
    uint32_t budget = client->send_timeout_ms != 0u ? client->send_timeout_ms :
        client->config.operation_timeout_ms != 0u ? client->config.operation_timeout_ms : 1000u;
    *deadline = h2_pal_time_deadline_ms(current, budget);
    return 1;
}

static int32_t send_prefix(h2_pal_mqtt_client_t *client, const uint8_t *buffer, size_t length,
                           uint64_t deadline) {
    const h2_pal_net_api_t *net = client->provider->config.net;
    h2_pal_net_socket_t socket = client->tls_socket >= 0 ? client->tls_socket : client->socket;
    size_t sent = 0u;
    while (sent < length) {
        uint64_t current = 0u;
        int rc = h2_pal_time_get_monotonic_ms(client->time_api, &current);
        if (rc != H2_PAL_OK) {client->send_result = rc;break;}
        if (current >= deadline) {client->send_result = H2_PAL_ERR_TIMEOUT;break;}
        uint32_t remaining = (uint32_t)(deadline - current);
        rc = net != NULL && net->vtable != NULL && net->vtable->tcp_send_timeout != NULL
            ? h2_pal_net_tcp_send_timeout(net, socket, buffer + sent, length - sent, remaining)
            : h2_pal_net_tcp_send(net, socket, buffer + sent, length - sent);
        if (rc > 0) {
            if ((size_t)rc > length - sent) {client->send_result = H2_PAL_ERR_IO;break;}
            sent += (size_t)rc;
            continue;
        }
        if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_TIMEOUT) {
            client->send_result = rc == 0 ? H2_PAL_ERR_CLOSED : rc;
            break;
        }
        rc = h2_pal_time_sleep_ms(client->time_api, 1u);
        if (rc != H2_PAL_OK) {client->send_result = rc;break;}
    }
    return sent != 0u ? (int32_t)sent : client->send_result == H2_PAL_OK ? 0 : -1;
}

int32_t h2_coremqtt_transport_send(NetworkContext_t *network, const void *buffer, size_t length) {
    if (network == NULL || network->client == NULL || (buffer == NULL && length != 0u) ||
        length > (size_t)INT32_MAX) return -1;
    h2_pal_mqtt_client_t *client = network->client;
    uint64_t deadline = 0u;
    if (!transaction_start(client, &deadline)) return -1;
    return send_prefix(client, buffer, length, deadline);
}

int32_t h2_coremqtt_transport_writev(NetworkContext_t *network, TransportOutVector_t *vectors, size_t count) {
    if (network == NULL || network->client == NULL || vectors == NULL || count == 0u) return -1;
    size_t total = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if ((vectors[i].iov_base == NULL && vectors[i].iov_len != 0u) ||
            vectors[i].iov_len > (size_t)INT32_MAX - total) return -1;
        total += vectors[i].iov_len;
    }
    h2_pal_mqtt_client_t *client = network->client;
    uint64_t deadline = 0u;
    if (!transaction_start(client, &deadline)) return -1;
    int32_t sent = 0;
    /* One configured PAL deadline spans every vector, short write and retry.
     * No borrowed input is copied/retained. Core retry timing cannot truncate
     * a transaction that is still progressing within its operation deadline. */
    for (size_t i = 0u; i < count; ++i) {
        if (vectors[i].iov_len == 0u) continue;
        int32_t rc = send_prefix(client, vectors[i].iov_base, vectors[i].iov_len, deadline);
        if (rc <= 0) return sent > 0 ? sent : rc;
        sent += rc;
        if ((size_t)rc < vectors[i].iov_len || client->send_result != H2_PAL_OK) return sent;
    }
    return sent;
}
