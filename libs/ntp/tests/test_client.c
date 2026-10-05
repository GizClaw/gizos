#include "h2_ntp.h"

#include <assert.h>
#include <string.h>

#define SERVER_TIME_MS UINT64_C(1791230400000)

typedef struct fixture {
    uint64_t monotonic_ms;
    uint64_t wall_ms;
    uint32_t opens;
    uint32_t closes;
    uint32_t sends;
    uint32_t receives;
    uint32_t sets;
    uint32_t timeouts;
    int valid;
    int bad_reply;
    h2_pal_result_t status_result;
    h2_pal_result_t set_result;
    h2_pal_net_addr_t server;
    uint8_t request[H2_NTP_PACKET_SIZE];
} fixture_t;

static h2_pal_result_t monotonic(void *user, uint64_t *out) {
    *out = ((fixture_t *)user)->monotonic_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t wall(void *user, uint64_t *out) {
    *out = ((fixture_t *)user)->wall_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t status(void *user, h2_pal_time_wall_status_t *out) {
    fixture_t *f = user;
    out->valid = (uint8_t)f->valid;
    out->source = f->valid ? H2_PAL_TIME_WALL_SOURCE_RTC
                          : H2_PAL_TIME_WALL_SOURCE_BOOT_DEFAULT;
    return f->status_result;
}

static h2_pal_result_t set_wall(void *user, uint64_t ms) {
    fixture_t *f = user;
    ++f->sets;
    if (f->set_result == H2_PAL_OK) {
        f->wall_ms = ms;
        f->valid = 1;
    }
    return f->set_result;
}

static int open_udp(void *user, h2_pal_net_family_t family, uint16_t port,
                    h2_pal_net_socket_t *out, h2_pal_net_addr_t *bind) {
    fixture_t *f = user;
    assert(family == H2_PAL_NET_FAMILY_IPV4 && port == 0u);
    ++f->opens;
    *out = 7;
    memset(bind, 0, sizeof(*bind));
    bind->family = family;
    return H2_PAL_OK;
}

static int send_udp(void *user, h2_pal_net_socket_t socket,
                    const h2_pal_net_addr_t *to, const uint8_t *data, size_t len) {
    fixture_t *f = user;
    assert(socket == 7 && len == H2_NTP_PACKET_SIZE);
    assert(to->port == H2_NTP_PORT);
    assert(memcmp(to->ip, f->server.ip, 4u) == 0);
    assert(f->sets == 0u);
    ++f->sends;
    memcpy(f->request, data, len);
    return (int)len;
}

static int receive_udp(void *user, h2_pal_net_socket_t socket,
                       h2_pal_net_addr_t *from, uint8_t *data, size_t len,
                       uint32_t timeout_ms) {
    fixture_t *f = user;
    assert(socket == 7 && len == H2_NTP_PACKET_SIZE && timeout_ms <= 200u);
    ++f->receives;
    if (f->timeouts > 0u) {
        --f->timeouts;
        f->monotonic_ms += timeout_ms;
        return H2_PAL_ERR_TIMEOUT;
    }
    f->monotonic_ms += 20u;
    *from = f->server;
    uint8_t *packet = data;
    /* Model a server reply: mode=server, stratum=2, echoed request timestamp,
     * and equal receive/transmit times so the fixture has 20 ms round trip. */
    uint8_t server_packet[H2_NTP_PACKET_SIZE];
    assert(h2_ntp_build_request(SERVER_TIME_MS, server_packet) == H2_NTP_OK);
    memset(packet, 0, len);
    packet[0] = 0x24u;
    packet[1] = f->bad_reply ? 0u : 2u;
    memcpy(packet + 24u, f->request + 40u, 8u);
    memcpy(packet + 32u, server_packet + 40u, 8u);
    memcpy(packet + 40u, server_packet + 40u, 8u);
    return (int)len;
}

static void close_udp(void *user, h2_pal_net_socket_t socket) {
    assert(socket == 7);
    ++((fixture_t *)user)->closes;
}

static int sync(fixture_t *f, uint8_t retries, h2_ntp_sync_result_t *result) {
    const h2_pal_time_vtable_t time_vtable = {
        .get_monotonic_ms = monotonic,
        .get_wall_ms = wall,
        .get_wall_status = status,
        .set_wall_ms = set_wall,
    };
    const h2_pal_time_api_t time = {.user = f, .vtable = &time_vtable};
    const h2_pal_net_vtable_t net_vtable = {
        .udp_open = open_udp,
        .udp_sendto = send_udp,
        .udp_recvfrom = receive_udp,
        .close = close_udp,
    };
    const h2_pal_net_api_t net = {.user = f, .vtable = &net_vtable};
    f->server = (h2_pal_net_addr_t){
        .family = H2_PAL_NET_FAMILY_IPV4, .port = H2_NTP_PORT,
        .ip = {192u, 0u, 2u, 1u},
    };
    const h2_ntp_client_config_t config = {
        .net = &net, .time = &time, .server = f->server,
        .timeout_ms = 200u, .retries = retries, .set_wall_clock = 1u,
    };
    return h2_ntp_sync(&config, result);
}

int main(void) {
    h2_ntp_sync_result_t result = {0};
    fixture_t cold = {.monotonic_ms = 123456u};
    assert(sync(&cold, 0u, &result) == H2_NTP_OK);
    assert(cold.valid && cold.sets == 1u);
    assert(cold.wall_ms == SERVER_TIME_MS + 10u);
    assert(result.wall_clock_set && result.applied_wall_ms == cold.wall_ms);
    assert(cold.opens == 1u && cold.closes == 1u);

    fixture_t warm = {
        .monotonic_ms = 123456u, .wall_ms = SERVER_TIME_MS - 10000u,
        .valid = 1,
    };
    assert(sync(&warm, 0u, &result) == H2_NTP_OK);
    assert(warm.wall_ms == SERVER_TIME_MS + 10u && warm.sets == 1u);

    fixture_t timeout = {.monotonic_ms = 123456u, .timeouts = 3u};
    assert(sync(&timeout, 2u, &result) == H2_NTP_ERR_TIMEOUT);
    assert(!timeout.valid && timeout.sets == 0u);
    assert(timeout.opens == 3u && timeout.closes == 3u);

    fixture_t retry = {.monotonic_ms = 123456u, .timeouts = 1u};
    assert(sync(&retry, 1u, &result) == H2_NTP_OK);
    assert(retry.valid && retry.opens == 2u && retry.closes == 2u);

    fixture_t bad = {.monotonic_ms = 123456u, .bad_reply = 1};
    assert(sync(&bad, 0u, &result) == H2_NTP_ERR_UNSYNCED);
    assert(!bad.valid && bad.sets == 0u && bad.closes == 1u);

    fixture_t clock_error = {.status_result = H2_PAL_ERR_IO};
    assert(sync(&clock_error, 0u, &result) == H2_NTP_ERR_UNSUPPORTED);
    assert(clock_error.opens == 0u && clock_error.sets == 0u);

    fixture_t unsupported = {
        .monotonic_ms = 123456u, .set_result = H2_PAL_ERR_UNSUPPORTED,
    };
    assert(sync(&unsupported, 0u, &result) == H2_NTP_OK_TIME_SET_UNSUPPORTED);
    assert(!unsupported.valid && !result.wall_clock_set);
    return 0;
}
