#include "h2_iperf_internal.h"

#include <string.h>

#define H2_IPERF_SERVER_DEFAULT_CONTROL_TIMEOUT_MS 30000u
#define H2_IPERF_SERVER_STREAM_ACCEPT_TIMEOUT_MS 10000u
#define H2_IPERF_SERVER_RESULTS_JSON_CAP 1024u
#define H2_IPERF_SERVER_UDP_RECV_CAP 65536u
#define H2_IPERF_SERVER_SCTP_SHUTDOWN_MS 500u
#define H2_IPERF_SERVER_FALLBACK_DURATION_MS 3600000u

struct h2_iperf_server {
    h2_iperf_config_t config;
    h2_pal_net_family_t family;
    h2_pal_net_bind_t bind;
    bool has_bind;
    uint16_t port;
    uint16_t sctp_udp_port;
    uint16_t sctp_packet_size;
    uint32_t control_timeout_ms;
    h2_pal_net_socket_t listener;
    h2_pal_net_socket_t sctp_udp;
    uint32_t random_state;
};

/** Parameters negotiated with one client. */
typedef struct session_params {
    h2_iperf_protocol_t protocol;
    bool reverse;
    bool bidirectional;
    bool counters_64bit;
    int64_t parallel;
    int64_t time_s;
    int64_t bytes;
    int64_t blockcount;
    int64_t len;
    int64_t bandwidth;
} session_params_t;

typedef struct session {
    h2_iperf_server_t *server;
    const h2_iperf_config_t *config;
    h2_pal_net_socket_t ctrl;
    char cookie[H2_IPERF_COOKIE_SIZE];
    session_params_t params;
    h2_iperf_stream_t stream;
    bool stream_ready;
    uint8_t *block;
    size_t block_len;
    h2_iperf_result_t *result;
} session_t;

h2_pal_result_t h2_iperf_server_create(
    const h2_iperf_config_t *config,
    const h2_iperf_server_params_t *params,
    h2_iperf_server_t **out_server) {
    if (out_server == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_server = NULL;
    if (!h2_iperf_config_is_valid(config) || params == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_iperf_server_t *server = h2_pal_mem_alloc(config->mem, sizeof(*server));
    if (server == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(server, 0, sizeof(*server));
    server->config = *config;
    server->family = params->family == H2_PAL_NET_FAMILY_IPV6
        ? H2_PAL_NET_FAMILY_IPV6
        : H2_PAL_NET_FAMILY_IPV4;
    if (params->bind != NULL) {
        server->bind = *params->bind;
        server->has_bind = true;
    }
    server->sctp_packet_size = params->sctp_packet_size != 0u
        ? params->sctp_packet_size
        : H2_IPERF_DEFAULT_SCTP_PACKET_SIZE;
    server->control_timeout_ms = params->control_timeout_ms != 0u
        ? params->control_timeout_ms
        : H2_IPERF_SERVER_DEFAULT_CONTROL_TIMEOUT_MS;
    server->listener = -1;
    server->sctp_udp = -1;
    server->random_state = h2_iperf_random_seed(config);

    uint16_t port = params->ephemeral_port
        ? 0u
        : (params->port != 0u ? params->port : (uint16_t)H2_IPERF_DEFAULT_PORT);
    h2_pal_net_addr_t bound;
    int rc = h2_pal_net_tcp_listen(
        config->net, server->family, port,
        server->has_bind ? &server->bind : NULL, &server->listener, &bound);
    if (rc != H2_PAL_OK) {
        h2_pal_mem_free(config->mem, server);
        return (h2_pal_result_t)rc;
    }
    server->port = bound.port;

    if (config->sctp != NULL) {
        uint16_t sctp_port = params->ephemeral_sctp_udp_port
            ? 0u
            : (params->sctp_udp_port != 0u
                   ? params->sctp_udp_port
                   : (uint16_t)H2_IPERF_DEFAULT_SCTP_UDP_PORT);
        rc = h2_pal_net_udp_open_bound(
            config->net, server->family, sctp_port,
            server->has_bind ? &server->bind : NULL, &server->sctp_udp, &bound);
        if (rc != H2_PAL_OK) {
            h2_pal_net_close(config->net, server->listener);
            h2_pal_mem_free(config->mem, server);
            return (h2_pal_result_t)rc;
        }
        server->sctp_udp_port = bound.port;
    }
    *out_server = server;
    return H2_PAL_OK;
}

uint16_t h2_iperf_server_port(const h2_iperf_server_t *server) {
    return server == NULL ? 0u : server->port;
}

uint16_t h2_iperf_server_sctp_udp_port(const h2_iperf_server_t *server) {
    return server == NULL ? 0u : server->sctp_udp_port;
}

void h2_iperf_server_destroy(h2_iperf_server_t **server) {
    if (server == NULL || *server == NULL) {
        return;
    }
    h2_iperf_server_t *self = *server;
    if (self->listener >= 0) {
        h2_pal_net_close(self->config.net, self->listener);
    }
    if (self->sctp_udp >= 0) {
        h2_pal_net_close(self->config.net, self->sctp_udp);
    }
    h2_pal_mem_free(self->config.mem, self);
    *server = NULL;
}

/* ---- one client session -------------------------------------------------- */

static bool session_parse_params(session_t *session, const char *json, size_t len) {
    session_params_t *p = &session->params;
    memset(p, 0, sizeof(*p));
    p->parallel = 1;
    if (h2_iperf_json_get_true(json, len, "udp")) {
        p->protocol = H2_IPERF_PROTOCOL_UDP;
    } else if (h2_iperf_json_get_true(json, len, "sctp")) {
        p->protocol = H2_IPERF_PROTOCOL_SCTP;
    } else if (h2_iperf_json_get_true(json, len, "tcp")) {
        p->protocol = H2_IPERF_PROTOCOL_TCP;
    } else {
        return false;
    }
    p->reverse = h2_iperf_json_get_true(json, len, "reverse");
    p->bidirectional = h2_iperf_json_get_true(json, len, "bidirectional");
    int64_t value = 0;
    if (h2_iperf_json_get_i64(json, len, "udp_counters_64bit", &value)) {
        p->counters_64bit = value != 0;
    }
    (void)h2_iperf_json_get_i64(json, len, "parallel", &p->parallel);
    (void)h2_iperf_json_get_i64(json, len, "time", &p->time_s);
    (void)h2_iperf_json_get_i64(json, len, "num", &p->bytes);
    (void)h2_iperf_json_get_i64(json, len, "blockcount", &p->blockcount);
    (void)h2_iperf_json_get_i64(json, len, "len", &p->len);
    (void)h2_iperf_json_get_i64(json, len, "bandwidth", &p->bandwidth);
    return true;
}

static int32_t session_reject_reason(const session_t *session) {
    const session_params_t *p = &session->params;
    if (p->protocol == H2_IPERF_PROTOCOL_SCTP && session->config->sctp == NULL) {
        return H2_IPERF_IE_NOSCTP;
    }
    if (p->parallel != 1 || p->bidirectional || p->blockcount != 0 ||
        p->len < 0 || p->len > (int64_t)(1024u * 1024u)) {
        return H2_IPERF_IE_UNIMP;
    }
    return 0;
}

static h2_pal_result_t session_accept_tcp_stream(session_t *session) {
    const h2_iperf_config_t *config = session->config;
    uint64_t deadline = h2_iperf_now_ms(config) + H2_IPERF_SERVER_STREAM_ACCEPT_TIMEOUT_MS;
    for (;;) {
        uint64_t now = h2_iperf_now_ms(config);
        if (h2_pal_time_deadline_expired(now, deadline)) {
            return H2_PAL_ERR_TIMEOUT;
        }
        h2_pal_net_socket_t sock = -1;
        h2_pal_result_t result = h2_pal_net_tcp_accept(
            config->net, session->server->listener, &sock, NULL, (uint32_t)(deadline - now));
        if (result == H2_PAL_ERR_WOULD_BLOCK) {
            continue;
        }
        if (result != H2_PAL_OK) {
            return result;
        }
        char cookie[H2_IPERF_COOKIE_SIZE];
        result = h2_iperf_ctrl_recv_all(
            config, sock, (uint8_t *)cookie, sizeof(cookie), session->server->control_timeout_ms);
        if (result == H2_PAL_OK && memcmp(cookie, session->cookie, sizeof(cookie)) == 0) {
            h2_iperf_stream_tcp_adopt(&session->stream, sock);
            session->stream_ready = true;
            return H2_PAL_OK;
        }
        /* Another client arrived mid-test; iperf3 answers ACCESS_DENIED. */
        (void)h2_iperf_ctrl_send_state(config, sock, H2_IPERF_STATE_ACCESS_DENIED, 1000u);
        h2_pal_net_close(config->net, sock);
    }
}

static h2_pal_result_t session_accept_sctp_stream(session_t *session) {
    const h2_iperf_config_t *config = session->config;
    uint8_t cookie[H2_IPERF_COOKIE_SIZE];
    h2_iperf_stream_sctp_capture(&session->stream, cookie, sizeof(cookie));
    h2_pal_result_t result = h2_iperf_stream_sctp_accept(
        &session->stream, session->server->port, session->server->sctp_packet_size,
        session->block_len, H2_IPERF_SERVER_STREAM_ACCEPT_TIMEOUT_MS);
    if (result != H2_PAL_OK) {
        return result;
    }
    uint64_t deadline = h2_iperf_now_ms(config) + session->server->control_timeout_ms;
    while (!session->stream.capture_done) {
        uint64_t now = h2_iperf_now_ms(config);
        if (h2_pal_time_deadline_expired(now, deadline)) {
            return H2_PAL_ERR_TIMEOUT;
        }
        result = h2_iperf_stream_sctp_pump(&session->stream, 20u);
        if (result != H2_PAL_OK) {
            return result;
        }
    }
    if (session->stream.capture_len != sizeof(cookie) ||
        memcmp(cookie, session->cookie, sizeof(cookie)) != 0) {
        return H2_PAL_ERR_BUSY;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t session_open_stream(session_t *session) {
    const h2_iperf_config_t *config = session->config;
    h2_iperf_server_t *server = session->server;
    h2_iperf_stream_init(&session->stream, config, session->params.protocol, &server->random_state);
    session->stream.counters_64bit = session->params.counters_64bit;
    switch (session->params.protocol) {
    case H2_IPERF_PROTOCOL_TCP:
        return H2_PAL_OK;
    case H2_IPERF_PROTOCOL_UDP: {
        h2_pal_result_t result = h2_iperf_stream_udp_open(
            &session->stream, server->family, server->port,
            server->has_bind ? &server->bind : NULL, NULL);
        if (result == H2_PAL_OK) {
            session->stream_ready = true;
        }
        return result;
    }
    case H2_IPERF_PROTOCOL_SCTP:
        h2_iperf_stream_udp_adopt(&session->stream, server->sctp_udp, false);
        session->stream_ready = true;
        return H2_PAL_OK;
    default:
        return H2_PAL_ERR_UNSUPPORTED;
    }
}

static h2_pal_result_t session_accept_stream(session_t *session) {
    switch (session->params.protocol) {
    case H2_IPERF_PROTOCOL_TCP:
        return session_accept_tcp_stream(session);
    case H2_IPERF_PROTOCOL_UDP:
        return h2_iperf_stream_udp_accept(
            &session->stream, session->block, session->block_len,
            H2_IPERF_SERVER_STREAM_ACCEPT_TIMEOUT_MS);
    case H2_IPERF_PROTOCOL_SCTP:
        return session_accept_sctp_stream(session);
    default:
        return H2_PAL_ERR_UNSUPPORTED;
    }
}

static void session_close_stream(session_t *session) {
    if (!session->stream_ready) {
        return;
    }
    if (session->params.protocol == H2_IPERF_PROTOCOL_SCTP) {
        (void)h2_iperf_stream_sctp_shutdown(&session->stream, H2_IPERF_SERVER_SCTP_SHUTDOWN_MS);
    }
    h2_iperf_stream_close(&session->stream);
    session->stream_ready = false;
}

static void session_stats(
    const session_t *session,
    const h2_iperf_run_t *run,
    h2_iperf_stream_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->bytes = run->bytes;
    stats->packets = run->packets;
    stats->lost_packets = -1;
    stats->retransmits = -1;
    stats->duration_ms = (uint32_t)((run->end_us - run->start_us) / 1000u);
    if (!session->params.reverse && session->params.protocol == H2_IPERF_PROTOCOL_UDP) {
        stats->lost_packets = session->stream.udp_errors;
        stats->out_of_order = session->stream.udp_out_of_order;
        stats->jitter_ms = session->stream.jitter_s * 1000.0;
        stats->packets = session->stream.udp_rx_seq;
    }
}

static h2_pal_result_t session_run_data(session_t *session, int8_t *out_ctrl_state) {
    const session_params_t *p = &session->params;
    uint64_t safety_ms = p->time_s > 0
        ? (uint64_t)p->time_s * 1000u + H2_IPERF_SERVER_GRACE_MS
        : H2_IPERF_SERVER_FALLBACK_DURATION_MS;
    uint64_t bitrate = p->bandwidth > 0 ? (uint64_t)p->bandwidth : 0u;
    if (bitrate == 0u && p->protocol == H2_IPERF_PROTOCOL_UDP) {
        bitrate = H2_IPERF_DEFAULT_UDP_BITRATE;
    }
    h2_iperf_run_t run = {
        .stream = &session->stream,
        .ctrl = session->ctrl,
        .block = session->block,
        .block_len = p->reverse ? (size_t)p->len : session->block_len,
        .bitrate_bps = p->reverse ? bitrate : 0u,
        .duration_ms = safety_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)safety_ms,
        .byte_limit = 0u,
    };
    session->stream.udp_account = !p->reverse;
    h2_pal_result_t result = p->reverse
        ? h2_iperf_run_sender(&run)
        : h2_iperf_run_receiver(&run);
    session_stats(session, &run, &session->result->local);
    *out_ctrl_state = run.ctrl_state;
    if (result != H2_PAL_OK) {
        return result;
    }
    if (run.ctrl_closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (run.ctrl_state == 0) {
        /* Neither TEST_END nor a termination arrived before the grace period. */
        return H2_PAL_ERR_TIMEOUT;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t session_exchange_results(session_t *session) {
    const h2_iperf_config_t *config = session->config;
    uint32_t timeout = session->server->control_timeout_ms;
    h2_pal_result_t result = h2_iperf_ctrl_send_state(
        config, session->ctrl, H2_IPERF_STATE_EXCHANGE_RESULTS, timeout);
    if (result != H2_PAL_OK) {
        return result;
    }
    char *peer_json = NULL;
    size_t peer_len = 0u;
    result = h2_iperf_ctrl_recv_json(config, session->ctrl, &peer_json, &peer_len, timeout);
    if (result != H2_PAL_OK) {
        return result;
    }
    bool parsed = h2_iperf_parse_results_json(peer_json, peer_len, &session->result->remote);
    h2_pal_mem_free(config->mem, peer_json);
    if (!parsed) {
        return H2_PAL_ERR_FORMAT;
    }
    char json[H2_IPERF_SERVER_RESULTS_JSON_CAP];
    if (!h2_iperf_build_results_json(
            json, sizeof(json), session->params.reverse, &session->result->local)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    result = h2_iperf_ctrl_send_json(config, session->ctrl, json, strlen(json), timeout);
    if (result != H2_PAL_OK) {
        return result;
    }
    result = h2_iperf_ctrl_send_state(
        config, session->ctrl, H2_IPERF_STATE_DISPLAY_RESULTS, timeout);
    if (result != H2_PAL_OK) {
        return result;
    }
    int8_t state = 0;
    (void)h2_iperf_ctrl_recv_state(config, session->ctrl, &state, timeout);
    return H2_PAL_OK;
}

static h2_pal_result_t session_run(session_t *session) {
    const h2_iperf_config_t *config = session->config;
    h2_iperf_server_t *server = session->server;
    uint32_t timeout = server->control_timeout_ms;
    h2_pal_result_t result = h2_iperf_ctrl_recv_all(
        config, session->ctrl, (uint8_t *)session->cookie, H2_IPERF_COOKIE_SIZE, timeout);
    if (result != H2_PAL_OK) {
        return result;
    }
    memcpy(session->result->cookie, session->cookie, H2_IPERF_COOKIE_SIZE);
    result = h2_iperf_ctrl_send_state(config, session->ctrl, H2_IPERF_STATE_PARAM_EXCHANGE, timeout);
    if (result != H2_PAL_OK) {
        return result;
    }
    char *json = NULL;
    size_t json_len = 0u;
    result = h2_iperf_ctrl_recv_json(config, session->ctrl, &json, &json_len, timeout);
    if (result != H2_PAL_OK) {
        return result;
    }
    bool parsed = session_parse_params(session, json, json_len);
    h2_pal_mem_free(config->mem, json);
    if (!parsed) {
        (void)h2_iperf_ctrl_send_server_error(config, session->ctrl, H2_IPERF_IE_PROTOCOL, timeout);
        return H2_PAL_ERR_FORMAT;
    }
    int32_t reject = session_reject_reason(session);
    if (reject != 0) {
        h2_iperf_log(config, H2_PAL_LOG_WARN, "rejecting client request (iperf errno %ld)", (long)reject);
        (void)h2_iperf_ctrl_send_server_error(config, session->ctrl, reject, timeout);
        return H2_PAL_ERR_UNSUPPORTED;
    }
    session->result->protocol = session->params.protocol;
    session->result->reverse = session->params.reverse;

    size_t len = session->params.len > 0 ? (size_t)session->params.len : 0u;
    if (len == 0u) {
        len = session->params.protocol == H2_IPERF_PROTOCOL_UDP
            ? H2_IPERF_DEFAULT_UDP_BLOCK_LEN
            : session->params.protocol == H2_IPERF_PROTOCOL_SCTP
                ? H2_IPERF_DEFAULT_SCTP_BLOCK_LEN
                : H2_IPERF_DEFAULT_TCP_BLOCK_LEN;
        session->params.len = (int64_t)len;
    }
    session->block_len = len;
    if (session->params.protocol == H2_IPERF_PROTOCOL_UDP &&
        session->block_len < H2_IPERF_SERVER_UDP_RECV_CAP) {
        session->block_len = H2_IPERF_SERVER_UDP_RECV_CAP;
    }
    session->block = h2_pal_mem_alloc(config->mem, session->block_len);
    if (session->block == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_iperf_random_fill(config, &server->random_state, session->block, session->block_len);

    result = session_open_stream(session);
    if (result != H2_PAL_OK) {
        (void)h2_iperf_ctrl_send_server_error(config, session->ctrl, H2_IPERF_IE_PROTOCOL, timeout);
        return result;
    }
    result = h2_iperf_ctrl_send_state(config, session->ctrl, H2_IPERF_STATE_CREATE_STREAMS, timeout);
    if (result != H2_PAL_OK) {
        return result;
    }
    result = session_accept_stream(session);
    if (result != H2_PAL_OK) {
        h2_iperf_log(config, H2_PAL_LOG_ERROR, "stream accept failed (%d)", (int)result);
        return result;
    }
    result = h2_iperf_ctrl_send_state(config, session->ctrl, H2_IPERF_STATE_TEST_START, timeout);
    if (result == H2_PAL_OK) {
        result = h2_iperf_ctrl_send_state(config, session->ctrl, H2_IPERF_STATE_TEST_RUNNING, timeout);
    }
    if (result != H2_PAL_OK) {
        return result;
    }
    int8_t ctrl_state = 0;
    result = session_run_data(session, &ctrl_state);
    session_close_stream(session);
    if (result != H2_PAL_OK) {
        return result;
    }
    if (ctrl_state == H2_IPERF_STATE_CLIENT_TERMINATE) {
        return H2_PAL_ERR_CLOSED;
    }
    if (ctrl_state != H2_IPERF_STATE_TEST_END) {
        h2_iperf_log(config, H2_PAL_LOG_ERROR, "unexpected control state %d", (int)ctrl_state);
        return H2_PAL_ERR_FORMAT;
    }
    return session_exchange_results(session);
}

h2_pal_result_t h2_iperf_server_run_once(
    h2_iperf_server_t *server,
    uint32_t accept_timeout_ms,
    h2_iperf_result_t *out_result) {
    if (server == NULL || out_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const h2_iperf_config_t *config = &server->config;
    memset(out_result, 0, sizeof(*out_result));
    out_result->local.lost_packets = -1;
    out_result->local.retransmits = -1;
    out_result->remote.lost_packets = -1;
    out_result->remote.retransmits = -1;

    h2_pal_net_socket_t ctrl = -1;
    h2_pal_result_t result = h2_pal_net_tcp_accept(
        config->net, server->listener, &ctrl, NULL, accept_timeout_ms);
    if (result == H2_PAL_ERR_WOULD_BLOCK) {
        return H2_PAL_ERR_TIMEOUT;
    }
    if (result != H2_PAL_OK) {
        return result;
    }

    session_t session;
    memset(&session, 0, sizeof(session));
    session.server = server;
    session.config = config;
    session.ctrl = ctrl;
    session.result = out_result;
    result = session_run(&session);
    session_close_stream(&session);
    if (session.block != NULL) {
        h2_pal_mem_free(config->mem, session.block);
    }
    h2_pal_net_close(config->net, ctrl);
    return result;
}
