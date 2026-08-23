#include "h2_dns.h"

#include <string.h>

static int map_pal_result(int rc) {
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
        return H2_DNS_ERR_TIMEOUT;
    }
    if (rc == H2_PAL_ERR_UNSUPPORTED) {
        return H2_DNS_ERR_UNSUPPORTED;
    }
    return H2_DNS_ERR_TRANSPORT;
}

static int addr_matches(const h2_pal_net_addr_t *actual, const h2_pal_net_addr_t *expected) {
    if (actual == NULL || expected == NULL || actual->family != expected->family || actual->port != expected->port) {
        return 0;
    }
    size_t ip_len = expected->family == H2_PAL_NET_FAMILY_IPV4 ? 4u : 16u;
    return memcmp(actual->ip, expected->ip, ip_len) == 0;
}

static int should_keep_receiving(int rc) {
    return rc == H2_DNS_ERR_TXID_MISMATCH ||
        rc == H2_DNS_ERR_MALFORMED;
}

static uint32_t remaining_timeout_ms(uint32_t timeout_ms, uint64_t start_ms, uint64_t now_ms) {
    uint64_t elapsed_ms = h2_pal_time_elapsed_ms(start_ms, now_ms);
    if (elapsed_ms >= (uint64_t)timeout_ms) {
        return 0u;
    }
    return timeout_ms - (uint32_t)elapsed_ms;
}

int h2_dns_query(const h2_dns_client_config_t *config, const h2_dns_query_t *query) {
    if (config == NULL || query == NULL || config->net == NULL || config->crypto == NULL || config->time == NULL ||
        query->name == NULL ||
        (!query->probe_only && query->out_count == NULL) ||
        (!query->probe_only && query->answers == NULL && query->max_answers != 0u)) {
        return H2_DNS_ERR_INVALID_ARG;
    }
    if (!query->probe_only && query->max_answers == 0u) {
        return H2_DNS_ERR_NO_SPACE;
    }
    uint8_t request[H2_DNS_MAX_PACKET_SIZE];
    uint8_t response[H2_DNS_MAX_PACKET_SIZE];
    uint8_t txid_bytes[2];
    int rc = h2_pal_crypto_random(config->crypto, txid_bytes, sizeof(txid_bytes));
    if (rc != H2_PAL_OK) {
        return H2_DNS_ERR_UNSUPPORTED;
    }
    uint16_t txid = ((uint16_t)txid_bytes[0] << 8) | (uint16_t)txid_bytes[1];
    size_t request_len = 0u;
    rc = h2_dns_encode_query(query->name, query->type, txid, request, sizeof(request), &request_len);
    if (rc != H2_DNS_OK) {
        return rc;
    }

    uint32_t attempts = config->retries == 0u ? 1u : (uint32_t)config->retries + 1u;
    uint32_t timeout_ms = config->timeout_ms == 0u ? 1000u : config->timeout_ms;
    h2_pal_net_addr_t server = config->server;
    if (server.port == 0u) {
        server.port = H2_DNS_PORT;
    }
    int last_rc = H2_DNS_ERR_TIMEOUT;
    for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
        h2_pal_net_socket_t socket_fd = -1;
        h2_pal_net_addr_t bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        rc = h2_pal_net_udp_open_bound(
            config->net,
            server.family,
            0u,
            config->bind,
            &socket_fd,
            &bind_addr);
        if (rc != H2_PAL_OK) {
            return map_pal_result(rc);
        }
        rc = h2_pal_net_udp_sendto(config->net, socket_fd, &server, request, request_len);
        if (rc < 0) {
            h2_pal_net_close(config->net, socket_fd);
            last_rc = map_pal_result(rc);
            continue;
        }
        uint64_t mono_start_ms = 0u;
        rc = h2_pal_time_get_monotonic_ms(config->time, &mono_start_ms);
        if (rc != H2_PAL_OK) {
            h2_pal_net_close(config->net, socket_fd);
            return H2_DNS_ERR_UNSUPPORTED;
        }
        uint32_t recv_timeout_ms = timeout_ms;
        for (;;) {
            h2_pal_net_addr_t from;
            rc = h2_pal_net_udp_recvfrom(
                config->net,
                socket_fd,
                &from,
                response,
                sizeof(response),
                recv_timeout_ms);
            if (rc < 0) {
                last_rc = map_pal_result(rc);
                break;
            }
            uint64_t mono_now_ms = 0u;
            int time_rc = h2_pal_time_get_monotonic_ms(config->time, &mono_now_ms);
            if (time_rc != H2_PAL_OK) {
                last_rc = H2_DNS_ERR_UNSUPPORTED;
                break;
            }
            uint32_t remaining_ms = remaining_timeout_ms(timeout_ms, mono_start_ms, mono_now_ms);
            if (!addr_matches(&from, &server)) {
                last_rc = H2_DNS_ERR_TIMEOUT;
                if (remaining_ms == 0u) {
                    break;
                }
                recv_timeout_ms = remaining_ms;
                continue;
            }
            size_t count = 0u;
            h2_dns_answer_t probe_answer;
            h2_dns_answer_t *answers = query->probe_only ? &probe_answer : query->answers;
            size_t max_answers = query->probe_only ? 1u : query->max_answers;
            rc = h2_dns_parse_response(response, (size_t)rc, txid, query->name, query->type, answers, max_answers, &count);
            if (rc == H2_DNS_OK) {
                h2_pal_net_close(config->net, socket_fd);
                if (!query->probe_only) {
                    *query->out_count = count;
                }
                return H2_DNS_OK;
            }
            if (query->probe_only && rc == H2_DNS_ERR_NO_ANSWER) {
                h2_pal_net_close(config->net, socket_fd);
                return H2_DNS_OK;
            }
            last_rc = rc;
            if (!should_keep_receiving(rc) || remaining_ms == 0u) {
                break;
            }
            recv_timeout_ms = remaining_ms;
        }
        h2_pal_net_close(config->net, socket_fd);
    }
    return last_rc;
}
