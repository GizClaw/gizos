#include "h2_ntp.h"

#include <string.h>

static int map_pal_result(int rc) {
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
        return H2_NTP_ERR_TIMEOUT;
    }
    if (rc == H2_PAL_ERR_UNSUPPORTED) {
        return H2_NTP_ERR_UNSUPPORTED;
    }
    return H2_NTP_ERR_TRANSPORT;
}

static int addr_matches(const h2_pal_net_addr_t *actual, const h2_pal_net_addr_t *expected) {
    if (actual == NULL || expected == NULL || actual->family != expected->family || actual->port != expected->port) {
        return 0;
    }
    size_t ip_len = expected->family == H2_PAL_NET_FAMILY_IPV4 ? 4u : 16u;
    return memcmp(actual->ip, expected->ip, ip_len) == 0;
}

static int should_keep_receiving(int rc) {
    return rc == H2_NTP_ERR_TXID_MISMATCH || rc == H2_NTP_ERR_MALFORMED;
}

static uint32_t remaining_timeout_ms(uint32_t timeout_ms, uint64_t start_ms, uint64_t now_ms) {
    uint64_t elapsed_ms = h2_pal_time_elapsed_ms(start_ms, now_ms);
    if (elapsed_ms >= (uint64_t)timeout_ms) {
        return 0u;
    }
    return timeout_ms - (uint32_t)elapsed_ms;
}

static int apply_time_offset(uint64_t base_ms, int64_t offset_ms, uint64_t *out_ms) {
    if (out_ms == NULL) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    if (offset_ms >= 0) {
        uint64_t delta_ms = (uint64_t)offset_ms;
        if (UINT64_MAX - base_ms < delta_ms) {
            return H2_NTP_ERR_MALFORMED;
        }
        *out_ms = base_ms + delta_ms;
        return H2_NTP_OK;
    }
    uint64_t delta_ms = (uint64_t)(-(offset_ms + 1)) + 1u;
    if (base_ms < delta_ms) {
        return H2_NTP_ERR_MALFORMED;
    }
    *out_ms = base_ms - delta_ms;
    return H2_NTP_OK;
}

int h2_ntp_sync(const h2_ntp_client_config_t *config, h2_ntp_sync_result_t *out_result) {
    if (config == NULL || out_result == NULL || config->net == NULL || config->time == NULL) {
        return H2_NTP_ERR_INVALID_ARG;
    }
    h2_pal_net_addr_t server = config->server;
    if (server.port == 0u) {
        server.port = H2_NTP_PORT;
    }
    uint32_t attempts = config->retries == 0u ? 1u : (uint32_t)config->retries + 1u;
    uint32_t timeout_ms = config->timeout_ms == 0u ? 1000u : config->timeout_ms;
    int last_rc = H2_NTP_ERR_TIMEOUT;
    for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
        uint64_t wall_ms = 0u;
        uint64_t mono_start_ms = 0u;
        int rc = h2_pal_time_get_wall_ms(config->time, &wall_ms);
        if (rc != H2_PAL_OK) {
            return H2_NTP_ERR_UNSUPPORTED;
        }
        rc = h2_pal_time_get_monotonic_ms(config->time, &mono_start_ms);
        if (rc != H2_PAL_OK) {
            return H2_NTP_ERR_UNSUPPORTED;
        }
        uint8_t request[H2_NTP_PACKET_SIZE];
        uint8_t response[H2_NTP_PACKET_SIZE];
        int ntp_rc = h2_ntp_build_request(wall_ms, request);
        if (ntp_rc != H2_NTP_OK) {
            return ntp_rc;
        }
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
        rc = h2_pal_net_udp_sendto(config->net, socket_fd, &server, request, sizeof(request));
        if (rc < 0) {
            h2_pal_net_close(config->net, socket_fd);
            last_rc = map_pal_result(rc);
            continue;
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
            uint64_t mono_end_ms = 0u;
            int time_rc = h2_pal_time_get_monotonic_ms(config->time, &mono_end_ms);
            if (time_rc != H2_PAL_OK) {
                last_rc = H2_NTP_ERR_UNSUPPORTED;
                break;
            }
            uint32_t remaining_ms = remaining_timeout_ms(timeout_ms, mono_start_ms, mono_end_ms);
            if (!addr_matches(&from, &server)) {
                last_rc = H2_NTP_ERR_TIMEOUT;
                if (remaining_ms == 0u) {
                    break;
                }
                recv_timeout_ms = remaining_ms;
                continue;
            }
            if (rc != H2_NTP_PACKET_SIZE) {
                last_rc = H2_NTP_ERR_MALFORMED;
                if (remaining_ms == 0u) {
                    break;
                }
                recv_timeout_ms = remaining_ms;
                continue;
            }
            uint64_t elapsed_ms = h2_pal_time_elapsed_ms(mono_start_ms, mono_end_ms);
            if (UINT64_MAX - wall_ms < elapsed_ms) {
                last_rc = H2_NTP_ERR_MALFORMED;
                break;
            }
            uint64_t local_receive_wall_ms = wall_ms + elapsed_ms;
            ntp_rc = h2_ntp_parse_response(response, wall_ms, local_receive_wall_ms, mono_end_ms, out_result);
            if (ntp_rc != H2_NTP_OK) {
                last_rc = ntp_rc;
                if (!should_keep_receiving(ntp_rc) || remaining_ms == 0u) {
                    break;
                }
                recv_timeout_ms = remaining_ms;
                continue;
            }
            h2_pal_net_close(config->net, socket_fd);
            if (config->set_wall_clock) {
                uint64_t applied_wall_ms = 0u;
                ntp_rc = apply_time_offset(local_receive_wall_ms, out_result->offset_ms, &applied_wall_ms);
                if (ntp_rc != H2_NTP_OK) {
                    return ntp_rc;
                }
                rc = h2_pal_time_set_wall_ms(config->time, applied_wall_ms);
                if (rc == H2_PAL_OK) {
                    out_result->wall_clock_set = 1u;
                    out_result->applied_wall_ms = applied_wall_ms;
                    return H2_NTP_OK;
                }
                if (rc == H2_PAL_ERR_UNSUPPORTED) {
                    return H2_NTP_OK_TIME_SET_UNSUPPORTED;
                }
                return H2_NTP_ERR_TRANSPORT;
            }
            return H2_NTP_OK;
        }
        h2_pal_net_close(config->net, socket_fd);
    }
    return last_rc;
}
