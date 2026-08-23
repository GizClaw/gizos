#include "fake_mqtt_platform.h"

#include <stdlib.h>
#include <string.h>

static void *fake_alloc(void *user, size_t len) {
    (void)user;
    return calloc(1u, len);
}

static void *fake_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void fake_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static h2_pal_result_t fake_now(void *user, uint64_t *out_ms) {
    fake_mqtt_platform_t *fake = (fake_mqtt_platform_t *)user;
    *out_ms = fake->now_ms++;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_sleep(void *user, uint32_t ms) {
    fake_mqtt_platform_t *fake = (fake_mqtt_platform_t *)user;
    fake->now_ms += ms;
    return H2_PAL_OK;
}

static int fake_resolve(void *user, const char *host, h2_pal_net_addr_t *out_addr) {
    (void)user;
    if (host == NULL || out_addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->family = H2_PAL_NET_FAMILY_IPV4;
    out_addr->ip[0] = 127u;
    out_addr->ip[3] = 1u;
    return H2_PAL_OK;
}

static int fake_tcp_open(void *user, h2_pal_net_family_t family, h2_pal_net_socket_t *out_socket) {
    fake_mqtt_platform_t *fake = (fake_mqtt_platform_t *)user;
    if (family != H2_PAL_NET_FAMILY_IPV4 || out_socket == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    fake->socket_open = 1;
    *out_socket = 3;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_tcp_connect(
    void *user,
    h2_pal_net_socket_t socket,
    const h2_pal_net_addr_t *addr,
    uint32_t timeout_ms) {
    fake_mqtt_platform_t *fake = (fake_mqtt_platform_t *)user;
    (void)timeout_ms;
    if (socket < 0 || addr == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (fake->timeout_connect) {
        return H2_PAL_ERR_TIMEOUT;
    }
    return fake->fail_connect ? H2_PAL_ERR_IO : H2_PAL_OK;
}

void fake_mqtt_platform_push_rx(fake_mqtt_platform_t *fake, const uint8_t *data, size_t len) {
    if (fake == NULL || data == NULL || fake->rx_len + len > sizeof(fake->rx)) {
        abort();
    }
    memcpy(&fake->rx[fake->rx_len], data, len);
    fake->rx_len += len;
}

static void push_connack(fake_mqtt_platform_t *fake) {
    static const uint8_t packet[] = { 0x20u, 0x02u, 0x00u, 0x00u };
    fake_mqtt_platform_push_rx(fake, packet, sizeof(packet));
}

static void push_ack(fake_mqtt_platform_t *fake, uint8_t type, uint16_t packet_id, uint8_t code, int with_code) {
    uint8_t packet[5];
    packet[0] = type;
    packet[1] = with_code ? 0x03u : 0x02u;
    packet[2] = (uint8_t)(packet_id >> 8);
    packet[3] = (uint8_t)(packet_id & 0xffu);
    packet[4] = code;
    fake_mqtt_platform_push_rx(fake, packet, with_code ? 5u : 4u);
}

static uint16_t packet_id_at(const uint8_t *packet, size_t len, size_t index) {
    if (index + 1u >= len) {
        return 0u;
    }
    return (uint16_t)(((uint16_t)packet[index] << 8) | packet[index + 1u]);
}

static size_t decode_remaining_length(const uint8_t *packet, size_t len, size_t *out_header_len) {
    size_t multiplier = 1u;
    size_t value = 0u;
    size_t index = 1u;
    uint8_t encoded = 0u;
    do {
        if (index >= len) {
            return 0u;
        }
        encoded = packet[index++];
        value += (size_t)(encoded & 0x7fu) * multiplier;
        multiplier *= 128u;
    } while ((encoded & 0x80u) != 0u && index < 5u);
    *out_header_len = index;
    return value;
}

static void maybe_auto_respond(fake_mqtt_platform_t *fake) {
    if (!fake->auto_respond) {
        return;
    }
    while (fake->tx_scan_pos + 2u <= fake->tx_len) {
        const uint8_t *packet = &fake->tx[fake->tx_scan_pos];
        size_t available = fake->tx_len - fake->tx_scan_pos;
        size_t header_len = 0u;
        size_t remaining = decode_remaining_length(packet, available, &header_len);
        if (header_len == 0u || header_len + remaining > available) {
            return;
        }
        size_t frame_len = header_len + remaining;
        uint8_t type = packet[0] & 0xf0u;
        if (type == 0x10u) {
            push_connack(fake);
        } else if (type == 0x80u) {
            push_ack(fake, 0x90u, packet_id_at(packet, frame_len, header_len), 0x00u, 1);
        } else if (type == 0xa0u) {
            push_ack(fake, 0xb0u, packet_id_at(packet, frame_len, header_len), 0x00u, 0);
        } else if (type == 0x30u && (packet[0] & 0x06u) == 0x02u && frame_len >= header_len + 4u) {
            size_t topic_len = ((size_t)packet[header_len] << 8) | packet[header_len + 1u];
            size_t id_index = header_len + 2u + topic_len;
            push_ack(fake, 0x40u, packet_id_at(packet, frame_len, id_index), 0x00u, 0);
        } else if (type == 0xc0u) {
            static const uint8_t pingresp[] = { 0xd0u, 0x00u };
            fake_mqtt_platform_push_rx(fake, pingresp, sizeof(pingresp));
        }
        fake->tx_scan_pos += frame_len;
    }
}

static int fake_tcp_send(void *user, h2_pal_net_socket_t socket, const uint8_t *data, size_t len) {
    fake_mqtt_platform_t *fake = (fake_mqtt_platform_t *)user;
    if (socket < 0 || (data == NULL && len != 0u) || fake->tx_len + len > sizeof(fake->tx)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memcpy(&fake->tx[fake->tx_len], data, len);
    fake->tx_len += len;
    maybe_auto_respond(fake);
    return (int)len;
}

static int fake_tcp_recv(void *user, h2_pal_net_socket_t socket, uint8_t *data, size_t len, uint32_t timeout_ms) {
    fake_mqtt_platform_t *fake = (fake_mqtt_platform_t *)user;
    (void)timeout_ms;
    if (socket < 0 || data == NULL || len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (fake->rx_pos >= fake->rx_len) {
        return H2_PAL_ERR_TIMEOUT;
    }
    size_t available = fake->rx_len - fake->rx_pos;
    size_t to_copy = available < len ? available : len;
    memcpy(data, &fake->rx[fake->rx_pos], to_copy);
    fake->rx_pos += to_copy;
    if (fake->rx_pos == fake->rx_len) {
        fake->rx_pos = 0u;
        fake->rx_len = 0u;
    }
    return (int)to_copy;
}

static h2_pal_result_t fake_tls_wrap(
    void *user,
    h2_pal_net_socket_t tcp_socket,
    const h2_pal_net_tls_config_t *config,
    uint32_t timeout_ms,
    h2_pal_net_socket_t *out_tls_socket) {
    fake_mqtt_platform_t *fake = (fake_mqtt_platform_t *)user;
    (void)config;
    (void)timeout_ms;
    fake->tls_called++;
    if (!fake->tls_supported) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    if (fake->tls_verify_fail) {
        return H2_PAL_ERR_TLS_VERIFY;
    }
    *out_tls_socket = tcp_socket;
    return H2_PAL_OK;
}

static void fake_close(void *user, h2_pal_net_socket_t socket) {
    fake_mqtt_platform_t *fake = (fake_mqtt_platform_t *)user;
    (void)socket;
    fake->socket_open = 0;
    fake->close_count++;
}

void fake_mqtt_platform_push_publish(fake_mqtt_platform_t *fake, const char *topic, const uint8_t *payload, size_t payload_len) {
    size_t topic_len = strlen(topic);
    size_t remaining = 2u + topic_len + payload_len;
    if (remaining > 127u) {
        abort();
    }
    uint8_t header[4];
    header[0] = 0x30u;
    header[1] = (uint8_t)remaining;
    header[2] = (uint8_t)(topic_len >> 8);
    header[3] = (uint8_t)(topic_len & 0xffu);
    fake_mqtt_platform_push_rx(fake, header, sizeof(header));
    fake_mqtt_platform_push_rx(fake, (const uint8_t *)topic, topic_len);
    fake_mqtt_platform_push_rx(fake, payload, payload_len);
}

void fake_mqtt_platform_init(fake_mqtt_platform_t *fake) {
    memset(fake, 0, sizeof(*fake));
    static const h2_pal_mem_vtable_t allocator_vtable = {
        .alloc = fake_alloc,
        .realloc = fake_realloc,
        .free = fake_free,
    };
    static const h2_pal_time_vtable_t time_vtable = {
        .get_monotonic_ms = fake_now,
        .sleep_ms = fake_sleep,
    };
    fake->allocator.vtable = &allocator_vtable;
    fake->time.user = fake;
    fake->time.vtable = &time_vtable;
    static const h2_pal_net_vtable_t net_vtable = {
        .resolve_addr = fake_resolve,
        .tcp_open = fake_tcp_open,
        .tcp_connect = fake_tcp_connect,
        .tcp_send = fake_tcp_send,
        .tcp_recv = fake_tcp_recv,
        .tls_wrap = fake_tls_wrap,
        .close = fake_close,
    };
    fake->net.user = fake;
    fake->net.vtable = &net_vtable;
    fake->auto_respond = 1;
}
