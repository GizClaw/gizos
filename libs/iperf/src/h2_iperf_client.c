#include "h2_iperf_internal.h"

#include <string.h>

#define H2_IPERF_CLIENT_DEFAULT_CONNECT_TIMEOUT_MS 5000u
#define H2_IPERF_CLIENT_DEFAULT_CONTROL_TIMEOUT_MS 30000u
#define H2_IPERF_CLIENT_CONNECT_RETRY_MS 100u
#define H2_IPERF_PARAMS_JSON_CAP 1024u
#define H2_IPERF_RESULTS_JSON_CAP 1024u

typedef struct client {
    const h2_iperf_config_t *config;
    h2_iperf_client_params_t params;
    h2_pal_net_addr_t server;
    uint32_t random_state;
    char cookie[H2_IPERF_COOKIE_SIZE];
    h2_pal_net_socket_t ctrl;
    h2_iperf_stream_t stream;
    bool stream_ready;
    uint8_t *block;
    size_t block_len;
    size_t buffer_len;
    h2_iperf_result_t *result;
    bool data_done;
    bool have_pending_state;
    int8_t pending_state;
} client_t;

static size_t default_block_len(h2_iperf_protocol_t protocol) {
    switch (protocol) {
    case H2_IPERF_PROTOCOL_UDP:
        return H2_IPERF_DEFAULT_UDP_BLOCK_LEN;
    case H2_IPERF_PROTOCOL_SCTP:
        return H2_IPERF_DEFAULT_SCTP_BLOCK_LEN;
    default:
        return H2_IPERF_DEFAULT_TCP_BLOCK_LEN;
    }
}

static h2_pal_result_t client_connect_control(client_t *client) {
    const h2_iperf_config_t *config = client->config;
    uint64_t deadline = h2_iperf_now_ms(config) + client->params.connect_timeout_ms;
    for (;;) {
        h2_pal_net_socket_t sock = -1;
        int rc = h2_pal_net_tcp_open_bound(config->net, client->server.family, NULL, &sock);
        if (rc != H2_PAL_OK) {
            return (h2_pal_result_t)rc;
        }
        uint64_t now = h2_iperf_now_ms(config);
        uint32_t left = h2_pal_time_deadline_expired(now, deadline)
            ? 0u
            : (uint32_t)(deadline - now);
        h2_pal_result_t result = H2_PAL_ERR_TIMEOUT;
        while (left != 0u) {
            result = h2_pal_net_tcp_connect(config->net, sock, &client->server, left);
            if (result != H2_PAL_ERR_TIMEOUT && result != H2_PAL_ERR_WOULD_BLOCK) {
                break;
            }
            now = h2_iperf_now_ms(config);
            left = h2_pal_time_deadline_expired(now, deadline) ? 0u : (uint32_t)(deadline - now);
        }
        if (result == H2_PAL_OK) {
            client->ctrl = sock;
            return H2_PAL_OK;
        }
        h2_pal_net_close(config->net, sock);
        if (left == 0u || result == H2_PAL_ERR_UNSUPPORTED) {
            return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_ERR_TIMEOUT : result;
        }
        /* The server may still be starting; retry until the budget ends. */
        h2_iperf_sleep_ms(config, H2_IPERF_CLIENT_CONNECT_RETRY_MS);
    }
}

static h2_pal_result_t client_send_parameters(client_t *client) {
    const h2_iperf_client_params_t *params = &client->params;
    char json[H2_IPERF_PARAMS_JSON_CAP];
    h2_iperf_json_writer_t w;
    h2_iperf_json_init(&w, json, sizeof(json));
    h2_iperf_json_object_begin(&w);
    switch (params->protocol) {
    case H2_IPERF_PROTOCOL_UDP:
        h2_iperf_json_key(&w, "udp");
        break;
    case H2_IPERF_PROTOCOL_SCTP:
        h2_iperf_json_key(&w, "sctp");
        break;
    default:
        h2_iperf_json_key(&w, "tcp");
        break;
    }
    h2_iperf_json_bool(&w, true);
    h2_iperf_json_key(&w, "omit");
    h2_iperf_json_i64(&w, 0);
    h2_iperf_json_key(&w, "time");
    uint32_t seconds = params->bytes != 0u ? 0u : (params->duration_ms + 999u) / 1000u;
    h2_iperf_json_u64(&w, seconds);
    h2_iperf_json_key(&w, "num");
    h2_iperf_json_u64(&w, params->bytes);
    h2_iperf_json_key(&w, "blockcount");
    h2_iperf_json_i64(&w, 0);
    h2_iperf_json_key(&w, "parallel");
    h2_iperf_json_i64(&w, 1);
    if (params->reverse) {
        h2_iperf_json_key(&w, "reverse");
        h2_iperf_json_bool(&w, true);
    }
    h2_iperf_json_key(&w, "len");
    h2_iperf_json_u64(&w, client->block_len);
    if (params->bitrate_bps != 0u) {
        h2_iperf_json_key(&w, "bandwidth");
        h2_iperf_json_u64(&w, params->bitrate_bps);
    }
    h2_iperf_json_key(&w, "pacing_timer");
    h2_iperf_json_i64(&w, 1000);
    h2_iperf_json_key(&w, "client_version");
    h2_iperf_json_string(&w, H2_IPERF_CLIENT_VERSION);
    h2_iperf_json_object_end(&w);
    if (!h2_iperf_json_finish(&w)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    return h2_iperf_ctrl_send_json(
        client->config, client->ctrl, json, w.len, params->control_timeout_ms);
}

static h2_pal_result_t client_create_stream(client_t *client) {
    const h2_iperf_config_t *config = client->config;
    const h2_iperf_client_params_t *params = &client->params;
    h2_iperf_stream_t *stream = &client->stream;
    h2_pal_result_t result;
    switch (params->protocol) {
    case H2_IPERF_PROTOCOL_TCP:
        result = h2_iperf_stream_tcp_connect(stream, &client->server, params->connect_timeout_ms);
        if (result != H2_PAL_OK) {
            return result;
        }
        client->stream_ready = true;
        return h2_iperf_ctrl_send_all(
            config, stream->sock, (const uint8_t *)client->cookie,
            H2_IPERF_COOKIE_SIZE, params->control_timeout_ms);
    case H2_IPERF_PROTOCOL_UDP:
        result = h2_iperf_stream_udp_open(stream, client->server.family, 0u, NULL, NULL);
        if (result != H2_PAL_OK) {
            return result;
        }
        client->stream_ready = true;
        h2_iperf_stream_set_peer(stream, &client->server);
        return h2_iperf_stream_udp_connect(
            stream, client->block, client->buffer_len, params->connect_timeout_ms);
    case H2_IPERF_PROTOCOL_SCTP: {
        result = h2_iperf_stream_udp_open(stream, client->server.family, 0u, NULL, NULL);
        if (result != H2_PAL_OK) {
            return result;
        }
        client->stream_ready = true;
        h2_pal_net_addr_t encap = client->server;
        encap.port = params->sctp_udp_port;
        h2_iperf_stream_set_peer(stream, &encap);
        uint8_t port_bytes[2];
        h2_iperf_random_fill(config, &client->random_state, port_bytes, sizeof(port_bytes));
        uint16_t local_port = (uint16_t)(1024u + ((((uint16_t)port_bytes[0] << 8u) | port_bytes[1]) % 60000u));
        result = h2_iperf_stream_sctp_create(
            stream, H2_PAL_SCTP_ROLE_ACTIVE, local_port, client->server.port,
            params->sctp_packet_size, client->block_len);
        if (result != H2_PAL_OK) {
            return result;
        }
        result = h2_iperf_stream_sctp_connect(stream, params->connect_timeout_ms);
        if (result != H2_PAL_OK) {
            return result;
        }
        int sent = h2_iperf_stream_send(
            stream, (const uint8_t *)client->cookie, H2_IPERF_COOKIE_SIZE,
            params->control_timeout_ms);
        return sent < 0 ? (h2_pal_result_t)sent : H2_PAL_OK;
    }
    default:
        return H2_PAL_ERR_UNSUPPORTED;
    }
}

static void stats_from_run(
    const h2_iperf_run_t *run,
    const h2_iperf_stream_t *stream,
    bool receiver,
    h2_iperf_stream_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->bytes = run->bytes;
    stats->packets = run->packets;
    stats->lost_packets = -1;
    stats->retransmits = -1;
    stats->duration_ms = (uint32_t)((run->end_us - run->start_us) / 1000u);
    if (receiver && stream->protocol == H2_IPERF_PROTOCOL_UDP) {
        stats->lost_packets = stream->udp_errors;
        stats->out_of_order = stream->udp_out_of_order;
        stats->jitter_ms = stream->jitter_s * 1000.0;
        stats->packets = stream->udp_rx_seq;
    }
}

static h2_pal_result_t client_run_data(client_t *client) {
    const h2_iperf_client_params_t *params = &client->params;
    h2_iperf_run_t run = {
        .stream = &client->stream,
        .ctrl = client->ctrl,
        .block = client->block,
        .block_len = params->reverse ? client->buffer_len : client->block_len,
        .bitrate_bps = params->bitrate_bps,
        .duration_ms = params->bytes != 0u ? 0u : params->duration_ms,
        .byte_limit = params->bytes,
    };
    client->stream.udp_account = params->reverse;
    h2_pal_result_t result = params->reverse
        ? h2_iperf_run_receiver(&run)
        : h2_iperf_run_sender(&run);
    stats_from_run(&run, &client->stream, params->reverse, &client->result->local);
    client->data_done = true;
    if (params->reverse && params->protocol == H2_IPERF_PROTOCOL_UDP) {
        /* Nobody reads the data socket once the run ends, but the server may
         * still be sending. On zero-copy stacks such as lwIP every datagram
         * left in the socket mailbox pins a driver RX buffer, so an unread
         * mailbox can starve the control connection of the receive buffers
         * it needs for the results exchange. Release the socket now. */
        h2_iperf_stream_close(&client->stream);
        client->stream_ready = false;
    }
    if (result != H2_PAL_OK) {
        return result;
    }
    if (run.ctrl_closed) {
        return H2_PAL_ERR_CLOSED;
    }
    if (run.ctrl_state != 0) {
        client->have_pending_state = true;
        client->pending_state = run.ctrl_state;
        return H2_PAL_OK;
    }
    return h2_iperf_ctrl_send_state(
        client->config, client->ctrl, H2_IPERF_STATE_TEST_END, params->control_timeout_ms);
}

static h2_pal_result_t client_exchange_results(client_t *client) {
    const h2_iperf_config_t *config = client->config;
    char json[H2_IPERF_RESULTS_JSON_CAP];
    if (!h2_iperf_build_results_json(
            json, sizeof(json), !client->params.reverse, &client->result->local)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    h2_pal_result_t result = h2_iperf_ctrl_send_json(
        config, client->ctrl, json, strlen(json), client->params.control_timeout_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    char *peer_json = NULL;
    size_t peer_len = 0u;
    result = h2_iperf_ctrl_recv_json(
        config, client->ctrl, &peer_json, &peer_len, client->params.control_timeout_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    bool parsed = h2_iperf_parse_results_json(peer_json, peer_len, &client->result->remote);
    h2_pal_mem_free(config->mem, peer_json);
    return parsed ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t client_handle_state(client_t *client, int8_t state, bool *done) {
    const h2_iperf_config_t *config = client->config;
    switch (state) {
    case H2_IPERF_STATE_PARAM_EXCHANGE:
        return client_send_parameters(client);
    case H2_IPERF_STATE_CREATE_STREAMS:
        return client_create_stream(client);
    case H2_IPERF_STATE_TEST_START:
        return H2_PAL_OK;
    case H2_IPERF_STATE_TEST_RUNNING:
        if (client->data_done) {
            return H2_PAL_OK;
        }
        return client_run_data(client);
    case H2_IPERF_STATE_EXCHANGE_RESULTS:
        return client_exchange_results(client);
    case H2_IPERF_STATE_DISPLAY_RESULTS:
        *done = true;
        return h2_iperf_ctrl_send_state(
            config, client->ctrl, H2_IPERF_STATE_IPERF_DONE, client->params.control_timeout_ms);
    case H2_IPERF_STATE_IPERF_DONE:
        *done = true;
        return H2_PAL_OK;
    case H2_IPERF_STATE_SERVER_ERROR: {
        uint8_t payload[8];
        int32_t iperf_errno = 0;
        int32_t sys_errno = 0;
        if (h2_iperf_ctrl_recv_all(
                config, client->ctrl, payload, sizeof(payload),
                client->params.control_timeout_ms) == H2_PAL_OK) {
            iperf_errno = (int32_t)h2_iperf_read_u32_be(payload);
            sys_errno = (int32_t)h2_iperf_read_u32_be(payload + 4);
        }
        h2_iperf_log(config, H2_PAL_LOG_ERROR,
                     "server error iperf_errno=%ld errno=%ld",
                     (long)iperf_errno, (long)sys_errno);
        return iperf_errno == H2_IPERF_IE_UNIMP || iperf_errno == H2_IPERF_IE_NOSCTP
            ? H2_PAL_ERR_UNSUPPORTED
            : H2_PAL_ERR_IO;
    }
    case H2_IPERF_STATE_ACCESS_DENIED:
        return H2_PAL_ERR_BUSY;
    case H2_IPERF_STATE_SERVER_TERMINATE:
        return H2_PAL_ERR_CLOSED;
    default:
        h2_iperf_log(config, H2_PAL_LOG_ERROR, "unexpected control state %d", (int)state);
        return H2_PAL_ERR_FORMAT;
    }
}

static h2_pal_result_t client_resolve(client_t *client) {
    const h2_iperf_client_params_t *params = &client->params;
    if (params->server_host != NULL) {
        int rc = h2_pal_net_resolve_addr(client->config->net, params->server_host, &client->server);
        if (rc != H2_PAL_OK) {
            return (h2_pal_result_t)rc;
        }
    } else {
        client->server = params->server_addr;
    }
    if (client->server.family != H2_PAL_NET_FAMILY_IPV4 &&
        client->server.family != H2_PAL_NET_FAMILY_IPV6) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    client->server.port = params->port;
    return H2_PAL_OK;
}

h2_pal_result_t h2_iperf_client_run(
    const h2_iperf_config_t *config,
    const h2_iperf_client_params_t *params,
    h2_iperf_result_t *out_result) {
    if (!h2_iperf_config_is_valid(config) || params == NULL || out_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (params->protocol != H2_IPERF_PROTOCOL_TCP &&
        params->protocol != H2_IPERF_PROTOCOL_UDP &&
        params->protocol != H2_IPERF_PROTOCOL_SCTP) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (params->protocol == H2_IPERF_PROTOCOL_SCTP && config->sctp == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    client_t client;
    memset(&client, 0, sizeof(client));
    client.config = config;
    client.params = *params;
    client.ctrl = -1;
    client.result = out_result;
    memset(out_result, 0, sizeof(*out_result));
    out_result->protocol = params->protocol;
    out_result->reverse = params->reverse;
    out_result->local.lost_packets = -1;
    out_result->local.retransmits = -1;
    out_result->remote.lost_packets = -1;
    out_result->remote.retransmits = -1;

    if (client.params.port == 0u) {
        client.params.port = H2_IPERF_DEFAULT_PORT;
    }
    if (client.params.duration_ms == 0u && client.params.bytes == 0u) {
        client.params.duration_ms = H2_IPERF_DEFAULT_DURATION_MS;
    }
    if (client.params.bitrate_bps == 0u && params->protocol == H2_IPERF_PROTOCOL_UDP) {
        client.params.bitrate_bps = H2_IPERF_DEFAULT_UDP_BITRATE;
    }
    if (client.params.sctp_udp_port == 0u) {
        client.params.sctp_udp_port = H2_IPERF_DEFAULT_SCTP_UDP_PORT;
    }
    if (client.params.sctp_packet_size == 0u) {
        client.params.sctp_packet_size = H2_IPERF_DEFAULT_SCTP_PACKET_SIZE;
    }
    if (client.params.connect_timeout_ms == 0u) {
        client.params.connect_timeout_ms = H2_IPERF_CLIENT_DEFAULT_CONNECT_TIMEOUT_MS;
    }
    if (client.params.control_timeout_ms == 0u) {
        client.params.control_timeout_ms = H2_IPERF_CLIENT_DEFAULT_CONTROL_TIMEOUT_MS;
    }
    client.block_len = params->block_len != 0u ? params->block_len : default_block_len(params->protocol);
    if (params->protocol == H2_IPERF_PROTOCOL_UDP &&
        (client.block_len < H2_IPERF_UDP_HEADER_32 ||
         (params->bytes != 0u && params->bytes < H2_IPERF_UDP_HEADER_32))) {
        /* Every UDP datagram carries a 12-byte header, so smaller budgets
         * cannot be represented exactly. */
        return H2_PAL_ERR_INVALID_ARG;
    }

    h2_pal_result_t result = client_resolve(&client);
    if (result != H2_PAL_OK) {
        return result;
    }
    client.random_state = h2_iperf_random_seed(config);
    h2_iperf_make_cookie(config, &client.random_state, client.cookie);
    memcpy(out_result->cookie, client.cookie, H2_IPERF_COOKIE_SIZE);
    h2_iperf_stream_init(&client.stream, config, params->protocol, &client.random_state);

    size_t buffer_len = client.block_len;
    if (params->protocol == H2_IPERF_PROTOCOL_UDP && buffer_len < 65536u && params->reverse) {
        buffer_len = 65536u; /* The server may answer with larger datagrams. */
    }
    client.block = h2_pal_mem_alloc(config->mem, buffer_len);
    if (client.block == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_iperf_random_fill(config, &client.random_state, client.block, buffer_len);
    client.buffer_len = buffer_len;

    result = client_connect_control(&client);
    if (result == H2_PAL_OK) {
        result = h2_iperf_ctrl_send_all(
            config, client.ctrl, (const uint8_t *)client.cookie, H2_IPERF_COOKIE_SIZE,
            client.params.control_timeout_ms);
    }
    bool done = false;
    while (result == H2_PAL_OK && !done) {
        int8_t state = 0;
        if (client.have_pending_state) {
            state = client.pending_state;
            client.have_pending_state = false;
        } else {
            result = h2_iperf_ctrl_recv_state(
                config, client.ctrl, &state, client.params.control_timeout_ms);
            if (result != H2_PAL_OK) {
                h2_iperf_log(config, H2_PAL_LOG_ERROR, "control read failed (%d)", (int)result);
                break;
            }
        }
        result = client_handle_state(&client, state, &done);
        if (result != H2_PAL_OK) {
            h2_iperf_log(config, H2_PAL_LOG_ERROR, "state %d failed (%d)", (int)state, (int)result);
        }
    }

    if (client.stream_ready) {
        if (params->protocol == H2_IPERF_PROTOCOL_SCTP) {
            (void)h2_iperf_stream_sctp_shutdown(&client.stream, 500u);
        }
        h2_iperf_stream_close(&client.stream);
    }
    if (client.ctrl >= 0) {
        h2_pal_net_close(config->net, client.ctrl);
    }
    h2_pal_mem_free(config->mem, client.block);
    return result;
}
