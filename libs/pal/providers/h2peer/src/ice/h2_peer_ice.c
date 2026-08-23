#include "h2_peer_ice.h"

#include <string.h>

static int h2_peer_ice_has_prefix(
    h2_pal_webrtc_str_t value,
    const char *prefix,
    size_t prefix_len) {
    return value.len >= prefix_len &&
           memcmp(value.data, prefix, prefix_len) == 0;
}

h2_pal_result_t h2_peer_ice_validate_server(
    const h2_pal_webrtc_ice_server_t *server) {
    if (server == NULL || server->url.data == NULL || server->url.len == 0u ||
        server->url.len > 512u ||
        (server->username.data == NULL && server->username.len != 0u) ||
        (server->credential.data == NULL && server->credential.len != 0u) ||
        server->username.len > 256u || server->credential.len > 256u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    static const char stun[] = "stun:";
    static const char turn[] = "turn:";
    static const char turns[] = "turns:";
    if (h2_peer_ice_has_prefix(
            server->url, turns, sizeof(turns) - 1u)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    int is_stun = h2_peer_ice_has_prefix(
        server->url, stun, sizeof(stun) - 1u);
    int is_turn = h2_peer_ice_has_prefix(
        server->url, turn, sizeof(turn) - 1u);
    if (!is_stun && !is_turn) {
        return H2_PAL_ERR_FORMAT;
    }
    if (is_turn &&
        (server->username.len == 0u || server->credential.len == 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    const char *query = memchr(server->url.data, '?', server->url.len);
    if (query != NULL) {
        static const char udp_query[] = "?transport=udp";
        size_t query_len = server->url.len -
                           (size_t)(query - server->url.data);
        if (query_len != sizeof(udp_query) - 1u ||
            memcmp(query, udp_query, sizeof(udp_query) - 1u) != 0) {
            return H2_PAL_ERR_UNSUPPORTED;
        }
    }
    return H2_PAL_OK;
}
