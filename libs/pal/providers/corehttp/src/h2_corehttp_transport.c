#include "h2_corehttp_internal.h"

#include <limits.h>

static h2_pal_result_t exchange_remaining_ms(
    h2_corehttp_exchange_t *exchange,
    uint32_t *out_remaining_ms) {
    if (exchange == NULL || out_remaining_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
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
    uint32_t slice = exchange->provider->config.io_slice_ms;
    if (slice == 0u || (uint64_t)slice > remaining) {
        slice = (uint32_t)remaining;
    }
    if (slice == 0u) {
        return H2_PAL_ERR_TIMEOUT;
    }
    *out_remaining_ms = slice;
    return H2_PAL_OK;
}

int32_t h2_corehttp_transport_send(
    NetworkContext_t *network,
    const void *buffer,
    size_t bytes_to_send) {
    if (network == NULL || network->exchange == NULL ||
        (buffer == NULL && bytes_to_send != 0u) ||
        bytes_to_send > (size_t)INT32_MAX) {
        return -1;
    }
    h2_corehttp_exchange_t *exchange = network->exchange;
    const uint8_t *bytes = (const uint8_t *)buffer;
    size_t offset = 0u;
    while (offset < bytes_to_send) {
        uint32_t timeout_ms = 0u;
        h2_pal_result_t rc = exchange_remaining_ms(exchange, &timeout_ms);
        if (rc != H2_PAL_OK) {
            exchange->result = rc;
            return -1;
        }
        int sent = h2_pal_net_tcp_send_timeout(
            exchange->provider->config.net,
            exchange->socket,
            bytes + offset,
            bytes_to_send - offset,
            timeout_ms);
        if (sent == H2_PAL_ERR_TIMEOUT || sent == H2_PAL_ERR_WOULD_BLOCK) {
            continue;
        }
        if (sent <= 0) {
            exchange->result = (h2_pal_result_t)sent;
            if (sent == 0) {
                exchange->result = H2_PAL_ERR_CLOSED;
            }
            return -1;
        }
        offset += (size_t)sent;
    }
    return (int32_t)offset;
}

int32_t h2_corehttp_transport_recv(
    NetworkContext_t *network,
    void *buffer,
    size_t bytes_to_recv) {
    if (network == NULL || network->exchange == NULL || buffer == NULL ||
        bytes_to_recv == 0u || bytes_to_recv > (size_t)INT32_MAX) {
        return -1;
    }
    h2_corehttp_exchange_t *exchange = network->exchange;
    uint32_t timeout_ms = 0u;
    h2_pal_result_t rc = exchange_remaining_ms(exchange, &timeout_ms);
    if (rc != H2_PAL_OK) {
        exchange->result = rc;
        return -1;
    }
    int received = h2_pal_net_tcp_recv(
        exchange->provider->config.net,
        exchange->socket,
        (uint8_t *)buffer,
        bytes_to_recv,
        timeout_ms);
    if (received == H2_PAL_ERR_TIMEOUT || received == H2_PAL_ERR_WOULD_BLOCK) {
        return 0;
    }
    if (received <= 0) {
        exchange->result = received == 0 ? H2_PAL_ERR_CLOSED
                                         : (h2_pal_result_t)received;
        return -1;
    }
    return received;
}
