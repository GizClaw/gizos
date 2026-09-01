#include "h2_iperf_internal.h"

#include <limits.h>
#include <string.h>

#define H2_IPERF_SCTP_MIN_PACKET_BUF 2048u
#define H2_IPERF_SCTP_EMIT_FAILURE_LIMIT 1000u
#define H2_IPERF_SCTP_DRAIN_PACKETS 32u
#define H2_IPERF_SEND_SLICE_MS 100u

static uint32_t remaining_ms(uint64_t now_ms, uint64_t deadline_ms) {
    if (h2_pal_time_deadline_expired(now_ms, deadline_ms)) {
        return 0u;
    }
    uint64_t left = deadline_ms - now_ms;
    return left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
}

static bool addr_equal(const h2_pal_net_addr_t *a, const h2_pal_net_addr_t *b) {
    return a->family == b->family && a->port == b->port &&
           memcmp(a->ip, b->ip, sizeof(a->ip)) == 0;
}

/* ---- SCTP callbacks ------------------------------------------------------ */

static h2_pal_result_t sctp_emit(
    void *user,
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len) {
    h2_iperf_stream_t *stream = user;
    (void)association;
    if (!stream->peer_known) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    int sent = h2_pal_net_udp_sendto(
        stream->config->net, stream->sock, &stream->peer, packet, packet_len);
    if (sent >= 0) {
        stream->emit_failures = 0u;
        return H2_PAL_OK;
    }
    /* Transient datagram send failures (for example ENOBUFS on a flooded
     * loopback) are retried by the association; persistent ones are fatal. */
    if (++stream->emit_failures > H2_IPERF_SCTP_EMIT_FAILURE_LIMIT) {
        return H2_PAL_ERR_IO;
    }
    return H2_PAL_ERR_WOULD_BLOCK;
}

static void sctp_on_state(
    void *user,
    h2_pal_sctp_association_t *association,
    h2_pal_sctp_state_t state,
    h2_pal_result_t reason) {
    h2_iperf_stream_t *stream = user;
    (void)association;
    stream->assoc_state = state;
    stream->assoc_reason = reason;
}

static h2_pal_result_t sctp_on_message(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_received_message_t *message) {
    h2_iperf_stream_t *stream = user;
    (void)association;
    if (stream->capture_active) {
        size_t copy = message->len < stream->capture_cap ? message->len : stream->capture_cap;
        memcpy(stream->capture, message->data, copy);
        stream->capture_len = copy;
        stream->capture_active = false;
        stream->capture_done = true;
        return H2_PAL_OK;
    }
    stream->sctp_pending_bytes += message->len;
    stream->sctp_rx_messages++;
    return H2_PAL_OK;
}

static void sctp_on_stream_reset(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_stream_reset_event_t *event) {
    (void)user;
    (void)association;
    (void)event;
}

/* ---- lifecycle ----------------------------------------------------------- */

void h2_iperf_stream_init(
    h2_iperf_stream_t *stream,
    const h2_iperf_config_t *config,
    h2_iperf_protocol_t protocol,
    uint32_t *random_state) {
    memset(stream, 0, sizeof(*stream));
    stream->config = config;
    stream->protocol = protocol;
    stream->random_state = random_state;
    stream->sock = -1;
    stream->assoc_state = H2_PAL_SCTP_STATE_NEW;
    stream->assoc_reason = H2_PAL_OK;
    stream->next_deadline_ms = H2_PAL_SCTP_NO_DEADLINE;
    stream->udp_first_packet = true;
}

void h2_iperf_stream_close(h2_iperf_stream_t *stream) {
    if (stream->assoc != NULL) {
        (void)h2_pal_sctp_association_close(stream->config->sctp, &stream->assoc);
        stream->assoc = NULL;
    }
    if (stream->packet_buf != NULL) {
        h2_pal_mem_free(stream->config->mem, stream->packet_buf);
        stream->packet_buf = NULL;
    }
    if (stream->sock >= 0 && stream->owns_sock) {
        h2_pal_net_close(stream->config->net, stream->sock);
    }
    stream->sock = -1;
    stream->owns_sock = false;
}

/* ---- TCP ----------------------------------------------------------------- */

h2_pal_result_t h2_iperf_stream_tcp_connect(
    h2_iperf_stream_t *stream,
    const h2_pal_net_addr_t *addr,
    uint32_t timeout_ms) {
    const h2_iperf_config_t *config = stream->config;
    h2_pal_net_socket_t sock = -1;
    int rc = h2_pal_net_tcp_open_bound(config->net, addr->family, NULL, &sock);
    if (rc != H2_PAL_OK) {
        return (h2_pal_result_t)rc;
    }
    uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
    h2_pal_result_t result;
    for (;;) {
        uint32_t left = remaining_ms(h2_iperf_now_ms(config), deadline);
        result = h2_pal_net_tcp_connect(config->net, sock, addr, left);
        if (result == H2_PAL_OK) {
            break;
        }
        if ((result == H2_PAL_ERR_TIMEOUT || result == H2_PAL_ERR_WOULD_BLOCK) &&
            left != 0u) {
            continue;
        }
        h2_pal_net_close(config->net, sock);
        return result == H2_PAL_ERR_WOULD_BLOCK ? H2_PAL_ERR_TIMEOUT : result;
    }
    stream->sock = sock;
    stream->owns_sock = true;
    stream->peer = *addr;
    stream->peer_known = true;
    return H2_PAL_OK;
}

void h2_iperf_stream_tcp_adopt(h2_iperf_stream_t *stream, h2_pal_net_socket_t sock) {
    stream->sock = sock;
    stream->owns_sock = true;
}

/* ---- UDP ----------------------------------------------------------------- */

h2_pal_result_t h2_iperf_stream_udp_open(
    h2_iperf_stream_t *stream,
    h2_pal_net_family_t family,
    uint16_t port,
    const h2_pal_net_bind_t *bind,
    uint16_t *out_port) {
    h2_pal_net_socket_t sock = -1;
    h2_pal_net_addr_t bound;
    int rc = h2_pal_net_udp_open_bound(
        stream->config->net, family, port, bind, &sock, &bound);
    if (rc != H2_PAL_OK) {
        return (h2_pal_result_t)rc;
    }
    stream->sock = sock;
    stream->owns_sock = true;
    if (out_port != NULL) {
        *out_port = bound.port;
    }
    return H2_PAL_OK;
}

void h2_iperf_stream_udp_adopt(
    h2_iperf_stream_t *stream,
    h2_pal_net_socket_t sock,
    bool owns_sock) {
    stream->sock = sock;
    stream->owns_sock = owns_sock;
}

void h2_iperf_stream_set_peer(h2_iperf_stream_t *stream, const h2_pal_net_addr_t *peer) {
    stream->peer = *peer;
    stream->peer_known = true;
}

h2_pal_result_t h2_iperf_stream_udp_connect(
    h2_iperf_stream_t *stream,
    uint8_t *scratch,
    size_t scratch_len,
    uint32_t timeout_ms) {
    const h2_iperf_config_t *config = stream->config;
    uint8_t hello[4];
    if (scratch_len < 4u || !stream->peer_known) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_iperf_write_u32_le(hello, H2_IPERF_UDP_CONNECT_MSG);
    int sent = h2_pal_net_udp_sendto(
        config->net, stream->sock, &stream->peer, hello, sizeof(hello));
    if (sent < 0) {
        return (h2_pal_result_t)sent;
    }
    uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
    /* In reverse mode data may already be flowing; iperf3 tolerates a few
     * datagrams before the reply. */
    for (unsigned attempt = 0u; attempt < 8u; ++attempt) {
        uint32_t left = remaining_ms(h2_iperf_now_ms(config), deadline);
        if (left == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        h2_pal_net_addr_t from;
        int got = h2_pal_net_udp_recvfrom(
            config->net, stream->sock, &from, scratch, scratch_len, left);
        if (got == H2_PAL_ERR_WOULD_BLOCK) {
            continue;
        }
        if (got < 0) {
            return (h2_pal_result_t)got;
        }
        if (got >= 4) {
            uint32_t word = h2_iperf_read_u32_le(scratch);
            if (word == H2_IPERF_UDP_CONNECT_REPLY ||
                word == H2_IPERF_UDP_LEGACY_CONNECT_REPLY) {
                return H2_PAL_OK;
            }
        }
    }
    return H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_iperf_stream_udp_accept(
    h2_iperf_stream_t *stream,
    uint8_t *scratch,
    size_t scratch_len,
    uint32_t timeout_ms) {
    const h2_iperf_config_t *config = stream->config;
    uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
    for (;;) {
        uint32_t left = remaining_ms(h2_iperf_now_ms(config), deadline);
        if (left == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        h2_pal_net_addr_t from;
        int got = h2_pal_net_udp_recvfrom(
            config->net, stream->sock, &from, scratch, scratch_len, left);
        if (got == H2_PAL_ERR_WOULD_BLOCK) {
            continue;
        }
        if (got < 0) {
            return (h2_pal_result_t)got;
        }
        stream->peer = from;
        stream->peer_known = true;
        break;
    }
    uint8_t reply[4];
    h2_iperf_write_u32_le(reply, H2_IPERF_UDP_CONNECT_REPLY);
    int sent = h2_pal_net_udp_sendto(
        config->net, stream->sock, &stream->peer, reply, sizeof(reply));
    return sent < 0 ? (h2_pal_result_t)sent : H2_PAL_OK;
}

void h2_iperf_udp_write_header(
    uint8_t *buf,
    uint64_t now_us,
    uint64_t sequence,
    bool counters_64bit) {
    h2_iperf_write_u32_be(buf, (uint32_t)(now_us / 1000000u));
    h2_iperf_write_u32_be(buf + 4, (uint32_t)(now_us % 1000000u));
    if (counters_64bit) {
        h2_iperf_write_u64_be(buf + 8, sequence);
    } else {
        h2_iperf_write_u32_be(buf + 8, (uint32_t)sequence);
    }
}

void h2_iperf_udp_account(
    h2_iperf_stream_t *stream,
    const uint8_t *buf,
    size_t len,
    uint64_t now_us) {
    size_t header = stream->counters_64bit ? H2_IPERF_UDP_HEADER_64 : H2_IPERF_UDP_HEADER_32;
    if (len < header) {
        return;
    }
    uint32_t sec = h2_iperf_read_u32_be(buf);
    uint32_t usec = h2_iperf_read_u32_be(buf + 4);
    uint64_t sequence = stream->counters_64bit
        ? h2_iperf_read_u64_be(buf + 8)
        : (uint64_t)h2_iperf_read_u32_be(buf + 8);
    stream->udp_rx_packets++;
    if (sequence >= stream->udp_rx_seq + 1u) {
        if (sequence > stream->udp_rx_seq + 1u) {
            stream->udp_errors += (int64_t)((sequence - 1u) - stream->udp_rx_seq);
        }
        stream->udp_rx_seq = sequence;
    } else {
        stream->udp_out_of_order++;
        if (stream->udp_errors > 0) {
            stream->udp_errors--;
        }
    }
    /* RFC 1889 jitter; clocks need not be synchronized. */
    double sent_s = (double)sec + (double)usec / 1000000.0;
    double arrival_s = (double)now_us / 1000000.0;
    double transit = arrival_s - sent_s;
    if (stream->udp_first_packet) {
        stream->prev_transit_s = transit;
        stream->udp_first_packet = false;
    }
    double d = transit - stream->prev_transit_s;
    if (d < 0.0) {
        d = -d;
    }
    stream->prev_transit_s = transit;
    stream->jitter_s += (d - stream->jitter_s) / 16.0;
}

/* ---- SCTP ---------------------------------------------------------------- */

h2_pal_result_t h2_iperf_stream_sctp_create(
    h2_iperf_stream_t *stream,
    h2_pal_sctp_role_t role,
    uint16_t local_sctp_port,
    uint16_t remote_sctp_port,
    uint16_t packet_size,
    size_t max_message_size) {
    const h2_iperf_config_t *config = stream->config;
    if (config->sctp == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (packet_size < H2_PAL_SCTP_MIN_PACKET_SIZE) {
        packet_size = H2_IPERF_DEFAULT_SCTP_PACKET_SIZE;
    }
    if (max_message_size == 0u) {
        max_message_size = H2_IPERF_DEFAULT_SCTP_BLOCK_LEN;
    }
    if (stream->packet_buf == NULL) {
        stream->packet_cap = packet_size > H2_IPERF_SCTP_MIN_PACKET_BUF
            ? packet_size
            : H2_IPERF_SCTP_MIN_PACKET_BUF;
        stream->packet_buf = h2_pal_mem_alloc(config->mem, stream->packet_cap);
        if (stream->packet_buf == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
    }
    size_t receive_buffer = max_message_size * 2u;
    if (receive_buffer < 1500u) {
        receive_buffer = 1500u;
    }
    const h2_pal_sctp_association_config_t assoc_config = {
        .role = role,
        .local_port = local_sctp_port,
        .remote_port = remote_sctp_port,
        .inbound_streams = H2_IPERF_SCTP_STREAMS,
        .outbound_streams = H2_IPERF_SCTP_STREAMS,
        .max_packet_size = packet_size,
        .max_message_size = max_message_size,
        .send_buffer_size = max_message_size * 2u + (size_t)packet_size * 8u,
        .receive_buffer_size = receive_buffer,
        .cookie_lifetime_ms = H2_IPERF_SCTP_COOKIE_LIFETIME_MS,
        .callbacks = {
            .user = stream,
            .emit_packet = sctp_emit,
            .on_state = sctp_on_state,
            .on_message = sctp_on_message,
            .on_stream_reset = sctp_on_stream_reset,
        },
    };
    stream->assoc_state = H2_PAL_SCTP_STATE_NEW;
    stream->assoc_reason = H2_PAL_OK;
    stream->emit_failures = 0u;
    return h2_pal_sctp_association_create(config->sctp, &assoc_config, &stream->assoc);
}

static h2_pal_result_t sctp_feed(h2_iperf_stream_t *stream, const uint8_t *packet, size_t len, uint64_t now_ms) {
    if (len < 12u) {
        return H2_PAL_OK;
    }
    h2_pal_result_t result = h2_pal_sctp_association_input_packet(
        stream->config->sctp, stream->assoc, packet, len, now_ms);
    if (result == H2_PAL_ERR_WOULD_BLOCK || result == H2_PAL_ERR_FORMAT ||
        result == H2_PAL_ERR_INVALID_ARG) {
        /* Malformed or unexpected packets are dropped like a kernel would. */
        return H2_PAL_OK;
    }
    return result;
}

static h2_pal_result_t sctp_status(const h2_iperf_stream_t *stream) {
    if (stream->assoc_state == H2_PAL_SCTP_STATE_FAILED) {
        return stream->assoc_reason == H2_PAL_OK ? H2_PAL_ERR_IO : stream->assoc_reason;
    }
    if (stream->assoc_state == H2_PAL_SCTP_STATE_CLOSED) {
        return H2_PAL_ERR_CLOSED;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_iperf_stream_sctp_pump(
    h2_iperf_stream_t *stream,
    uint32_t timeout_ms) {
    const h2_iperf_config_t *config = stream->config;
    if (stream->assoc == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint64_t now = h2_iperf_now_ms(config);
    h2_pal_result_t result = h2_pal_sctp_association_service(
        config->sctp, stream->assoc, now, &stream->next_deadline_ms);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    result = sctp_status(stream);
    if (result != H2_PAL_OK) {
        return result;
    }
    uint32_t wait = timeout_ms;
    if (stream->next_deadline_ms != H2_PAL_SCTP_NO_DEADLINE) {
        uint32_t until_deadline = remaining_ms(now, stream->next_deadline_ms);
        if (until_deadline < wait) {
            wait = until_deadline;
        }
    }
    for (unsigned drained = 0u; drained < H2_IPERF_SCTP_DRAIN_PACKETS; ++drained) {
        h2_pal_net_addr_t from;
        int got = h2_pal_net_udp_recvfrom(
            config->net, stream->sock, &from, stream->packet_buf,
            stream->packet_cap, drained == 0u ? wait : 0u);
        if (got == H2_PAL_ERR_TIMEOUT || got == H2_PAL_ERR_WOULD_BLOCK) {
            break;
        }
        if (got < 0) {
            return (h2_pal_result_t)got;
        }
        if (!stream->peer_known) {
            stream->peer = from;
            stream->peer_known = true;
        } else if (!addr_equal(&from, &stream->peer)) {
            continue;
        }
        result = sctp_feed(stream, stream->packet_buf, (size_t)got, h2_iperf_now_ms(config));
        if (result != H2_PAL_OK) {
            return result;
        }
        result = sctp_status(stream);
        if (result != H2_PAL_OK) {
            return result;
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_iperf_stream_sctp_connect(
    h2_iperf_stream_t *stream,
    uint32_t timeout_ms) {
    const h2_iperf_config_t *config = stream->config;
    if (stream->assoc == NULL || !stream->peer_known) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
    h2_pal_result_t result = h2_pal_sctp_association_start(
        config->sctp, stream->assoc, h2_iperf_now_ms(config));
    if (result != H2_PAL_OK) {
        return result;
    }
    while (stream->assoc_state != H2_PAL_SCTP_STATE_CONNECTED) {
        uint32_t left = remaining_ms(h2_iperf_now_ms(config), deadline);
        if (left == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        result = h2_iperf_stream_sctp_pump(stream, left < 20u ? left : 20u);
        if (result != H2_PAL_OK) {
            return result;
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_iperf_stream_sctp_accept(
    h2_iperf_stream_t *stream,
    uint16_t local_sctp_port,
    uint16_t packet_size,
    size_t max_message_size,
    uint32_t timeout_ms) {
    const h2_iperf_config_t *config = stream->config;
    if (config->sctp == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    size_t cap = packet_size > H2_IPERF_SCTP_MIN_PACKET_BUF ? packet_size : H2_IPERF_SCTP_MIN_PACKET_BUF;
    if (stream->packet_buf == NULL) {
        stream->packet_buf = h2_pal_mem_alloc(config->mem, cap);
        if (stream->packet_buf == NULL) {
            return H2_PAL_ERR_NO_MEMORY;
        }
        stream->packet_cap = cap;
    }
    uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
    for (;;) {
        uint32_t left = remaining_ms(h2_iperf_now_ms(config), deadline);
        if (left == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        h2_pal_net_addr_t from;
        int got = h2_pal_net_udp_recvfrom(
            config->net, stream->sock, &from, stream->packet_buf,
            stream->packet_cap, left);
        if (got == H2_PAL_ERR_TIMEOUT || got == H2_PAL_ERR_WOULD_BLOCK) {
            continue;
        }
        if (got < 0) {
            return (h2_pal_result_t)got;
        }
        if (got < 12) {
            continue;
        }
        uint16_t src_port = (uint16_t)(((uint16_t)stream->packet_buf[0] << 8u) | stream->packet_buf[1]);
        uint16_t dst_port = (uint16_t)(((uint16_t)stream->packet_buf[2] << 8u) | stream->packet_buf[3]);
        if (dst_port != local_sctp_port || src_port == 0u) {
            continue;
        }
        stream->peer = from;
        stream->peer_known = true;
        h2_pal_result_t result = h2_iperf_stream_sctp_create(
            stream, H2_PAL_SCTP_ROLE_PASSIVE, local_sctp_port, src_port,
            packet_size, max_message_size);
        if (result != H2_PAL_OK) {
            return result;
        }
        uint64_t now = h2_iperf_now_ms(config);
        result = h2_pal_sctp_association_start(config->sctp, stream->assoc, now);
        if (result != H2_PAL_OK) {
            return result;
        }
        result = sctp_feed(stream, stream->packet_buf, (size_t)got, now);
        if (result != H2_PAL_OK) {
            return result;
        }
        break;
    }
    while (stream->assoc_state != H2_PAL_SCTP_STATE_CONNECTED) {
        uint32_t left = remaining_ms(h2_iperf_now_ms(config), deadline);
        if (left == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        h2_pal_result_t result = h2_iperf_stream_sctp_pump(stream, left < 20u ? left : 20u);
        if (result != H2_PAL_OK) {
            return result;
        }
    }
    return H2_PAL_OK;
}

void h2_iperf_stream_sctp_capture(
    h2_iperf_stream_t *stream,
    uint8_t *capture,
    size_t cap) {
    stream->capture = capture;
    stream->capture_cap = cap;
    stream->capture_len = 0u;
    stream->capture_active = true;
    stream->capture_done = false;
}

h2_pal_result_t h2_iperf_stream_sctp_shutdown(
    h2_iperf_stream_t *stream,
    uint32_t timeout_ms) {
    const h2_iperf_config_t *config = stream->config;
    if (stream->assoc == NULL) {
        return H2_PAL_OK;
    }
    if (stream->assoc_state == H2_PAL_SCTP_STATE_CONNECTED) {
        (void)h2_pal_sctp_association_shutdown(
            config->sctp, stream->assoc, h2_iperf_now_ms(config));
    }
    uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
    while (stream->assoc_state != H2_PAL_SCTP_STATE_CLOSED &&
           stream->assoc_state != H2_PAL_SCTP_STATE_FAILED) {
        uint32_t left = remaining_ms(h2_iperf_now_ms(config), deadline);
        if (left == 0u) {
            break;
        }
        if (h2_iperf_stream_sctp_pump(stream, left < 20u ? left : 20u) != H2_PAL_OK) {
            break;
        }
    }
    (void)h2_pal_sctp_association_close(config->sctp, &stream->assoc);
    stream->assoc = NULL;
    return H2_PAL_OK;
}

/* ---- send / receive ------------------------------------------------------ */

int h2_iperf_stream_send(
    h2_iperf_stream_t *stream,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    const h2_iperf_config_t *config = stream->config;
    if (len == 0u || len > (size_t)INT_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    switch (stream->protocol) {
    case H2_IPERF_PROTOCOL_TCP: {
        int sent = h2_pal_net_tcp_send_timeout(config->net, stream->sock, data, len, timeout_ms);
        if (sent == H2_PAL_ERR_UNSUPPORTED) {
            sent = h2_pal_net_tcp_send(config->net, stream->sock, data, len);
        }
        return sent;
    }
    case H2_IPERF_PROTOCOL_UDP: {
        uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
        for (;;) {
            int sent = h2_pal_net_udp_sendto(config->net, stream->sock, &stream->peer, data, len);
            if (sent >= 0) {
                return sent;
            }
            if (remaining_ms(h2_iperf_now_ms(config), deadline) == 0u) {
                return sent;
            }
            h2_iperf_sleep_ms(config, 1u);
        }
    }
    case H2_IPERF_PROTOCOL_SCTP: {
        if (stream->assoc == NULL) {
            return H2_PAL_ERR_INVALID_STATE;
        }
        uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
        const h2_pal_sctp_message_t message = {
            .data = data,
            .len = len,
            .stream_id = 0u,
            .ppid = 0u,
            .unordered = false,
            .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
            .reliability_value = 0u,
        };
        for (;;) {
            h2_pal_result_t status = sctp_status(stream);
            if (status != H2_PAL_OK) {
                return status;
            }
            bool writable = false;
            (void)h2_pal_sctp_association_is_writable(config->sctp, stream->assoc, &writable);
            if (writable) {
                h2_pal_result_t result = h2_pal_sctp_association_send_message(
                    config->sctp, stream->assoc, &message, h2_iperf_now_ms(config));
                if (result == H2_PAL_OK) {
                    return (int)len;
                }
                if (result != H2_PAL_ERR_WOULD_BLOCK && result != H2_PAL_ERR_FULL &&
                    result != H2_PAL_ERR_NO_SPACE) {
                    return result;
                }
            }
            uint32_t left = remaining_ms(h2_iperf_now_ms(config), deadline);
            if (left == 0u) {
                return H2_PAL_ERR_TIMEOUT;
            }
            h2_pal_result_t pumped = h2_iperf_stream_sctp_pump(stream, left < 5u ? left : 5u);
            if (pumped != H2_PAL_OK) {
                return pumped;
            }
        }
    }
    default:
        return H2_PAL_ERR_UNSUPPORTED;
    }
}

int h2_iperf_stream_recv(
    h2_iperf_stream_t *stream,
    uint8_t *buf,
    size_t cap,
    uint32_t timeout_ms) {
    const h2_iperf_config_t *config = stream->config;
    switch (stream->protocol) {
    case H2_IPERF_PROTOCOL_TCP: {
        int got = h2_pal_net_tcp_recv(config->net, stream->sock, buf, cap, timeout_ms);
        if (got == H2_PAL_ERR_WOULD_BLOCK) {
            return H2_PAL_ERR_TIMEOUT;
        }
        return got == 0 ? H2_PAL_ERR_CLOSED : got;
    }
    case H2_IPERF_PROTOCOL_UDP: {
        uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
        for (;;) {
            uint32_t left = remaining_ms(h2_iperf_now_ms(config), deadline);
            h2_pal_net_addr_t from;
            int got = h2_pal_net_udp_recvfrom(config->net, stream->sock, &from, buf, cap, left);
            if (got == H2_PAL_ERR_WOULD_BLOCK) {
                if (left == 0u) {
                    return H2_PAL_ERR_TIMEOUT;
                }
                continue;
            }
            if (got < 0) {
                return got;
            }
            if (stream->peer_known && !addr_equal(&from, &stream->peer)) {
                if (left == 0u) {
                    return H2_PAL_ERR_TIMEOUT;
                }
                continue;
            }
            if (stream->udp_account) {
                h2_iperf_udp_account(stream, buf, (size_t)got, h2_iperf_now_us(config));
            }
            return got;
        }
    }
    case H2_IPERF_PROTOCOL_SCTP: {
        uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
        for (;;) {
            if (stream->sctp_pending_bytes > 0u) {
                uint64_t pending = stream->sctp_pending_bytes;
                if (pending > (uint64_t)INT_MAX) {
                    pending = (uint64_t)INT_MAX;
                }
                stream->sctp_pending_bytes -= pending;
                return (int)pending;
            }
            uint32_t left = remaining_ms(h2_iperf_now_ms(config), deadline);
            h2_pal_result_t result = h2_iperf_stream_sctp_pump(stream, left);
            if (result != H2_PAL_OK) {
                return result;
            }
            if (stream->sctp_pending_bytes == 0u && left == 0u) {
                return H2_PAL_ERR_TIMEOUT;
            }
        }
    }
    default:
        return H2_PAL_ERR_UNSUPPORTED;
    }
}

/* ---- shared data phase --------------------------------------------------- */

static bool run_poll_ctrl(h2_iperf_run_t *run, uint64_t now_us, uint64_t *last_poll_us) {
    if (now_us - *last_poll_us < (uint64_t)H2_IPERF_CTRL_POLL_INTERVAL_MS * 1000u) {
        return false;
    }
    *last_poll_us = now_us;
    int8_t state = 0;
    h2_pal_result_t result = h2_iperf_ctrl_recv_state(
        run->stream->config, run->ctrl, &state, 0u);
    if (result == H2_PAL_OK) {
        run->ctrl_state = state;
        return true;
    }
    if (result == H2_PAL_ERR_CLOSED || result == H2_PAL_ERR_IO) {
        run->ctrl_closed = true;
        return true;
    }
    return false;
}

static bool run_limits_reached(const h2_iperf_run_t *run, uint64_t now_us) {
    if (run->duration_ms != 0u &&
        now_us - run->start_us >= (uint64_t)run->duration_ms * 1000u) {
        return true;
    }
    return run->byte_limit != 0u && run->bytes >= run->byte_limit;
}

h2_pal_result_t h2_iperf_run_sender(h2_iperf_run_t *run) {
    h2_iperf_stream_t *stream = run->stream;
    const h2_iperf_config_t *config = stream->config;
    size_t header = 0u;
    if (stream->protocol == H2_IPERF_PROTOCOL_UDP) {
        header = stream->counters_64bit ? H2_IPERF_UDP_HEADER_64 : H2_IPERF_UDP_HEADER_32;
        if (run->block_len < header) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    }
    run->start_us = h2_iperf_now_us(config);
    run->bytes = 0u;
    run->packets = 0u;
    run->ctrl_state = 0;
    run->ctrl_closed = false;
    uint64_t last_poll = run->start_us;
    size_t offset = 0u;
    h2_pal_result_t result = H2_PAL_OK;
    for (;;) {
        uint64_t now = h2_iperf_now_us(config);
        if (run_limits_reached(run, now) || run_poll_ctrl(run, now, &last_poll)) {
            break;
        }
        if (run->bitrate_bps != 0u) {
            uint64_t elapsed_us = now - run->start_us;
            uint64_t allowed_bits = (run->bitrate_bps * elapsed_us) / 1000000u;
            uint64_t sent_bits = run->bytes * 8u;
            if (sent_bits > allowed_bits) {
                uint64_t wait_us = ((sent_bits - allowed_bits) * 1000000u) / run->bitrate_bps;
                uint32_t wait_ms = (uint32_t)((wait_us + 999u) / 1000u);
                if (wait_ms == 0u) {
                    wait_ms = 1u;
                }
                if (wait_ms > H2_IPERF_CTRL_POLL_INTERVAL_MS) {
                    wait_ms = H2_IPERF_CTRL_POLL_INTERVAL_MS;
                }
                if (stream->protocol == H2_IPERF_PROTOCOL_SCTP) {
                    result = h2_iperf_stream_sctp_pump(stream, wait_ms);
                    if (result != H2_PAL_OK) {
                        break;
                    }
                } else {
                    h2_iperf_sleep_ms(config, wait_ms);
                }
                continue;
            }
        }
        /* Never overshoot a byte budget: the final block is truncated to the
         * remaining bytes (a UDP datagram still carries its full header). */
        size_t budget = run->block_len;
        if (run->byte_limit != 0u && run->byte_limit - run->bytes < budget) {
            budget = (size_t)(run->byte_limit - run->bytes);
        }
        int sent;
        if (stream->protocol == H2_IPERF_PROTOCOL_UDP) {
            if (budget < header) {
                budget = header;
            }
            h2_iperf_udp_write_header(run->block, now, stream->udp_tx_seq + 1u, stream->counters_64bit);
            sent = h2_iperf_stream_send(stream, run->block, budget, H2_IPERF_SEND_SLICE_MS);
            if (sent > 0) {
                stream->udp_tx_seq++;
                run->packets++;
            }
        } else {
            size_t chunk = run->block_len - offset;
            if (chunk > budget) {
                chunk = budget;
            }
            sent = h2_iperf_stream_send(
                stream, run->block + offset, chunk, H2_IPERF_SEND_SLICE_MS);
            if (sent > 0) {
                offset += (size_t)sent;
                bool budget_done = run->byte_limit != 0u &&
                                   run->bytes + (uint64_t)sent >= run->byte_limit;
                if (offset >= run->block_len || budget_done) {
                    offset = 0u;
                    run->packets++;
                }
            }
        }
        if (sent == H2_PAL_ERR_TIMEOUT || sent == H2_PAL_ERR_WOULD_BLOCK) {
            continue;
        }
        if (sent < 0) {
            result = (h2_pal_result_t)sent;
            break;
        }
        run->bytes += (uint64_t)sent;
    }
    run->end_us = h2_iperf_now_us(config);
    return result;
}

h2_pal_result_t h2_iperf_run_receiver(h2_iperf_run_t *run) {
    h2_iperf_stream_t *stream = run->stream;
    const h2_iperf_config_t *config = stream->config;
    run->start_us = h2_iperf_now_us(config);
    run->bytes = 0u;
    run->packets = 0u;
    run->ctrl_state = 0;
    run->ctrl_closed = false;
    uint64_t last_poll = run->start_us;
    uint64_t sctp_messages_before = stream->sctp_rx_messages;
    h2_pal_result_t result = H2_PAL_OK;
    for (;;) {
        uint64_t now = h2_iperf_now_us(config);
        if (run_limits_reached(run, now) || run_poll_ctrl(run, now, &last_poll)) {
            break;
        }
        uint32_t slice = H2_IPERF_CTRL_POLL_INTERVAL_MS;
        if (run->duration_ms != 0u) {
            uint64_t left_us = (uint64_t)run->duration_ms * 1000u - (now - run->start_us);
            uint32_t left_ms = (uint32_t)((left_us + 999u) / 1000u);
            if (left_ms < slice) {
                slice = left_ms;
            }
        }
        int got = h2_iperf_stream_recv(stream, run->block, run->block_len, slice);
        if (got == H2_PAL_ERR_TIMEOUT) {
            continue;
        }
        if (got == H2_PAL_ERR_CLOSED) {
            break;
        }
        if (got < 0) {
            result = (h2_pal_result_t)got;
            break;
        }
        run->bytes += (uint64_t)got;
        if (stream->protocol == H2_IPERF_PROTOCOL_UDP) {
            run->packets++;
        }
    }
    if (stream->protocol == H2_IPERF_PROTOCOL_SCTP) {
        run->packets = stream->sctp_rx_messages - sctp_messages_before;
    }
    run->end_us = h2_iperf_now_us(config);
    return result;
}
