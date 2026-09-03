#include "agent.h"
#include "ice.h"

// These tests use assertions for both checks and the operations under test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int test_resolve(
    void *user,
    const char *host,
    h2_pal_net_addr_t *out_addr) {
    (void)user;
    memset(out_addr, 0, sizeof(*out_addr));
    if (strcmp(host, "127.0.0.1") == 0) {
        out_addr->family = H2_PAL_NET_FAMILY_IPV4;
        out_addr->ip[0] = 127u;
        out_addr->ip[3] = 1u;
        return H2_PAL_OK;
    }
    if (strcmp(host, "2001:db8::1") == 0) {
        static const uint8_t ipv6[16] = {
            0x20u, 0x01u, 0x0du, 0xb8u, 0u, 0u, 0u, 0u,
            0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u,
        };
        out_addr->family = H2_PAL_NET_FAMILY_IPV6;
        memcpy(out_addr->ip, ipv6, sizeof(ipv6));
        return H2_PAL_OK;
    }
    if (strcmp(host, "invalid-family") == 0) {
        out_addr->family = (h2_pal_net_family_t)5;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_IO;
}

static const h2_pal_net_vtable_t test_net_vtable = {
    .resolve_addr = test_resolve,
};

static const h2_pal_net_api_t test_net = {
    .vtable = &test_net_vtable,
};

static h2_pal_result_t test_time_get_monotonic_ms(void *user,
                                                  uint64_t *out_ms) {
    *out_ms = *(uint64_t *)user;
    return H2_PAL_OK;
}

static const h2_pal_time_vtable_t test_time_vtable = {
    .get_monotonic_ms = test_time_get_monotonic_ms,
};

typedef struct test_connect_state {
    int opens;
    int connects;
    int closes;
} test_connect_state_t;

static int test_tcp_open_bound(
    void *user,
    h2_pal_net_family_t family,
    const h2_pal_net_bind_t *bind_config,
    h2_pal_net_socket_t *out_socket) {
    test_connect_state_t *state = user;
    assert(family == H2_PAL_NET_FAMILY_IPV4);
    assert(bind_config != NULL &&
           bind_config->type == H2_PAL_NET_BIND_SOURCE_ADDR);
    state->opens++;
    *out_socket = 7;
    return H2_PAL_OK;
}

static h2_pal_result_t test_tcp_connect_failure(
    void *user,
    h2_pal_net_socket_t socket_fd,
    const h2_pal_net_addr_t *remote_addr,
    uint32_t timeout_ms) {
    test_connect_state_t *state = user;
    assert(socket_fd == 7 && remote_addr != NULL && timeout_ms == 25u);
    state->connects++;
    return H2_PAL_ERR_IO;
}

static void test_tcp_close(void *user, h2_pal_net_socket_t socket_fd) {
    test_connect_state_t *state = user;
    assert(socket_fd == 7);
    state->closes++;
}

static void test_bounded_candidate_parser(void) {
    static char candidate[] =
        "a=candidate:foundation 1 udp 123 127.0.0.1 3478 typ host";
    IceCandidate parsed;
    memset(&parsed, 0, sizeof(parsed));
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               candidate,
               candidate + sizeof(candidate) - 1u) == 0);
    assert(strcmp(parsed.foundation, "foundation") == 0);
    assert(parsed.addr.port == 3478u);
    assert(parsed.addr.family == H2_PAL_NET_FAMILY_IPV4);
    assert(parsed.transport == ICE_TRANSPORT_UDP);
    assert(parsed.tcp_type == ICE_TCP_TYPE_NONE);

    static char tcp_candidate[] =
        "a=candidate:tcp 1 TCP 123 127.0.0.1 3478 typ host tcptype passive";
    memset(&parsed, 0, sizeof(parsed));
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               tcp_candidate,
               tcp_candidate + sizeof(tcp_candidate) - 1u) ==
           ICE_CANDIDATE_PARSE_OK);
    assert(parsed.transport == ICE_TRANSPORT_TCP);
    assert(parsed.tcp_type == ICE_TCP_TYPE_PASSIVE);

    static char active_candidate[] =
        "a=candidate:tcp 1 TCP 123 127.0.0.1 9 typ host tcptype active";
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               active_candidate,
               active_candidate + sizeof(active_candidate) - 1u) ==
           ICE_CANDIDATE_PARSE_UNSUPPORTED);
    static char missing_tcp_type[] =
        "a=candidate:tcp 1 TCP 123 127.0.0.1 9 typ host";
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               missing_tcp_type,
               missing_tcp_type + sizeof(missing_tcp_type) - 1u) ==
           ICE_CANDIDATE_PARSE_INVALID);
    static char duplicate_tcp_type[] =
        "a=candidate:tcp 1 TCP 123 127.0.0.1 9 typ host tcptype passive tcptype passive";
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               duplicate_tcp_type,
               duplicate_tcp_type + sizeof(duplicate_tcp_type) - 1u) ==
           ICE_CANDIDATE_PARSE_INVALID);
    static char udp_tcp_type[] =
        "a=candidate:udp 1 UDP 123 127.0.0.1 9 typ host tcptype passive";
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               udp_tcp_type,
               udp_tcp_type + sizeof(udp_tcp_type) - 1u) ==
           ICE_CANDIDATE_PARSE_INVALID);
    static char zero_port[] =
        "a=candidate:udp 1 UDP 123 127.0.0.1 0 typ host";
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               zero_port,
               zero_port + sizeof(zero_port) - 1u) ==
           ICE_CANDIDATE_PARSE_INVALID);
    static char unsupported_before_resolution[] =
        "a=candidate:tcp 1 TCP 123 ignored.invalid 9 typ host tcptype so";
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               unsupported_before_resolution,
               unsupported_before_resolution +
                   sizeof(unsupported_before_resolution) - 1u) ==
           ICE_CANDIDATE_PARSE_UNSUPPORTED);

    static char ipv6_candidate[] =
        "a=candidate:v6 1 udp 456 2001:db8::1 65535 typ relay";
    memset(&parsed, 0, sizeof(parsed));
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               ipv6_candidate,
               ipv6_candidate + sizeof(ipv6_candidate) - 1u) == 0);
    assert(parsed.addr.family == H2_PAL_NET_FAMILY_IPV6);
    assert(parsed.addr.port == 65535u);
    assert(parsed.addr.ip[0] == 0x20u);
    assert(parsed.addr.ip[15] == 1u);
    char description[256];
    memset(description, 0, sizeof(description));
    ice_candidate_to_description(&parsed, description, sizeof(description));
    assert(strstr(
               description,
               "2001:0db8:0000:0000:0000:0000:0000:0001 65535") != NULL);

    static char invalid_family_candidate[] =
        "a=candidate:bad 1 udp 123 invalid-family 3478 typ host";
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               invalid_family_candidate,
               invalid_family_candidate +
                   sizeof(invalid_family_candidate) - 1u) != 0);

    char oversized[600];
    memset(oversized, 'x', sizeof(oversized));
    assert(ice_candidate_from_description(
               &test_net,
               &parsed,
               oversized,
               oversized + sizeof(oversized)) != 0);
}

static void test_remote_description_keeps_udp_and_passive_tcp(void) {
    Agent agent;
    memset(&agent, 0, sizeof(agent));
    agent.net = &test_net;
    char sdp[] =
        "a=ice-ufrag:test\r\n"
        "a=ice-pwd:password\r\n"
        "a=candidate:same 1 TCP 123 127.0.0.1 9820 typ host tcptype passive\r\n"
        "a=candidate:same 1 UDP 456 127.0.0.1 3478 typ host\r\n"
        "a=candidate:skip 1 TCP 789 127.0.0.1 9 typ host tcptype active\r\n";

    assert(agent_set_remote_description(&agent, sdp) == 0);
    assert(agent.remote_candidates_count == 2);
    assert(agent.remote_candidates[0].transport == ICE_TRANSPORT_TCP);
    assert(agent.remote_candidates[1].transport == ICE_TRANSPORT_UDP);
    assert(agent.remote_candidates[1].addr.port == 3478u);
}

static void test_local_tcp_format_and_pair_order(void) {
    h2_pal_net_addr_t source;
    memset(&source, 0, sizeof(source));
    source.family = H2_PAL_NET_FAMILY_IPV4;
    source.ip[0] = 127u;
    source.ip[3] = 1u;
    source.port = 4000u;
    IceCandidate udp;
    IceCandidate tcp;
    memset(&udp, 0, sizeof(udp));
    memset(&tcp, 0, sizeof(tcp));
    ice_candidate_create(&udp, 1, ICE_CANDIDATE_TYPE_HOST, &source);
    ice_candidate_create_tcp_active(&tcp, 2, &source);
    assert(tcp.addr.port == 9u);
    assert(tcp.priority < udp.priority);
    char description[256] = {0};
    ice_candidate_to_description(&tcp, description, sizeof(description));
    assert(strstr(description, " TCP ") != NULL);
    assert(strstr(description, " 9 typ host tcptype active\r\n") != NULL);

    Agent agent;
    memset(&agent, 0, sizeof(agent));
    agent.local_candidates[0] = tcp;
    agent.local_candidates[1] = udp;
    agent.local_candidates_count = 2;
    IceCandidate remote_tcp = tcp;
    remote_tcp.tcp_type = ICE_TCP_TYPE_PASSIVE;
    remote_tcp.addr.port = 5000u;
    IceCandidate remote_udp = udp;
    remote_udp.addr.port = 5001u;
    agent.remote_candidates[0] = remote_tcp;
    agent.remote_candidates[1] = remote_udp;
    agent.remote_candidates_count = 2;
    assert(agent_update_candidate_pairs(&agent) == 0);
    assert(agent.candidate_pairs_num == 2);
    assert(agent.candidate_pairs[0].local->transport == ICE_TRANSPORT_UDP);
    assert(agent.candidate_pairs[1].local->transport == ICE_TRANSPORT_TCP);
    remote_tcp.tcp_type = ICE_TCP_TYPE_ACTIVE;
    assert(!ice_candidate_pair_is_compatible(&tcp, &remote_tcp));
}

static void test_trickle_pair_update_preserves_active_pair(void) {
    h2_pal_net_addr_t source;
    memset(&source, 0, sizeof(source));
    source.family = H2_PAL_NET_FAMILY_IPV4;
    source.ip[0] = 127u;
    source.ip[3] = 1u;
    source.port = 4000u;

    Agent agent;
    memset(&agent, 0, sizeof(agent));
    ice_candidate_create_tcp_active(
        &agent.local_candidates[0], 1, &source);
    ice_candidate_create(
        &agent.local_candidates[1], 2, ICE_CANDIDATE_TYPE_HOST, &source);
    agent.local_candidates_count = 2;
    agent.remote_candidates[0] = agent.local_candidates[0];
    agent.remote_candidates[0].tcp_type = ICE_TCP_TYPE_PASSIVE;
    agent.remote_candidates[0].addr.port = 5000u;
    agent.remote_candidates_count = 1;
    assert(agent_update_candidate_pairs(&agent) == 0);
    assert(agent.candidate_pairs_num == 1);
    agent.candidate_pairs[0].state = ICE_CANDIDATE_STATE_SUCCEEDED;
    agent.candidate_pairs[0].conncheck = 7;
    agent.selected_pair = &agent.candidate_pairs[0];
    agent.nominated_pair = &agent.candidate_pairs[0];
    agent.transport_pair = &agent.candidate_pairs[0];

    agent.remote_candidates[1] = agent.local_candidates[1];
    agent.remote_candidates[1].addr.port = 5001u;
    agent.remote_candidates_count = 2;
    assert(agent_update_candidate_pairs(&agent) == 0);

    assert(agent.candidate_pairs_num == 2);
    assert(agent.candidate_pairs[0].local->transport == ICE_TRANSPORT_UDP);
    assert(agent.candidate_pairs[1].local->transport == ICE_TRANSPORT_TCP);
    assert(agent.selected_pair == &agent.candidate_pairs[1]);
    assert(agent.nominated_pair == &agent.candidate_pairs[1]);
    assert(agent.transport_pair == &agent.candidate_pairs[1]);
    assert(agent.selected_pair->state == ICE_CANDIDATE_STATE_SUCCEEDED);
    assert(agent.selected_pair->conncheck == 7);
}

static void test_connect_failure_consumes_one_pair_per_poll(void) {
    test_connect_state_t state = {0};
    const h2_pal_net_vtable_t net_vtable = {
        .tcp_open_bound = test_tcp_open_bound,
        .tcp_connect = test_tcp_connect_failure,
        .close = test_tcp_close,
    };
    const h2_pal_net_api_t net = {
        .user = &state,
        .vtable = &net_vtable,
    };
    h2_pal_net_addr_t source;
    memset(&source, 0, sizeof(source));
    source.family = H2_PAL_NET_FAMILY_IPV4;
    source.ip[0] = 127u;
    source.ip[3] = 1u;

    Agent agent;
    memset(&agent, 0, sizeof(agent));
    agent.net = &net;
    agent.transport.tcp_socket.fd = -1;
    ice_candidate_create_tcp_active(
        &agent.local_candidates[0], 1, &source);
    agent.local_candidates_count = 1;
    for (int i = 0; i < 2; ++i) {
        agent.remote_candidates[i] = agent.local_candidates[0];
        agent.remote_candidates[i].tcp_type = ICE_TCP_TYPE_PASSIVE;
        agent.remote_candidates[i].addr.port = (uint16_t)(5000 + i);
    }
    agent.remote_candidates_count = 2;
    assert(agent_update_candidate_pairs(&agent) == 0);
    assert(agent.candidate_pairs_num == 2);

    assert(agent_select_candidate_pair(&agent, 25u) == 0);
    assert(state.opens == 1 && state.connects == 1 && state.closes == 1);
    assert(agent.candidate_pairs[0].state == ICE_CANDIDATE_STATE_FAILED);
    assert(agent.candidate_pairs[1].state == ICE_CANDIDATE_STATE_FROZEN);
    assert(agent_select_candidate_pair(&agent, 25u) == 0);
    assert(state.opens == 2 && state.connects == 2 && state.closes == 2);
}

static void test_connectivity_check_uses_wall_clock_deadline(void) {
    uint64_t now_ms = (uint64_t)UINT32_MAX + 1000u;
    h2_pal_time_api_t time = {
        .user = &now_ms,
        .vtable = &test_time_vtable,
    };
    Agent agent;
    memset(&agent, 0, sizeof(agent));
    agent.time = &time;
    agent.candidate_pairs_num = 1;
    agent.candidate_pairs[0].state = ICE_CANDIDATE_STATE_INPROGRESS;
    agent.nominated_pair = &agent.candidate_pairs[0];
    agent.transport_pair = &agent.candidate_pairs[0];
    agent.ice_activity_time_ms = now_ms;

    now_ms += 1999u;
    assert(agent_select_candidate_pair(&agent, 0u) == 0);
    assert(agent.candidate_pairs[0].state ==
           ICE_CANDIDATE_STATE_INPROGRESS);

    now_ms++;
    assert(agent_select_candidate_pair(&agent, 0u) != 0);
    assert(agent.candidate_pairs[0].state == ICE_CANDIDATE_STATE_FAILED);
    assert(agent.transport_pair == NULL);
}

static void test_candidate_limit(void) {
    Agent agent;
    memset(&agent, 0, sizeof(agent));
    agent.net = &test_net;
    char sdp[2048];
    size_t used = 0u;
    for (int i = 0; i < AGENT_MAX_CANDIDATES + 1; ++i) {
        int written = snprintf(
            sdp + used,
            sizeof(sdp) - used,
            "a=candidate:%d 1 udp 123 127.0.0.1 %d typ host\r\n",
            i,
            3400 + i);
        assert(written > 0 && (size_t)written < sizeof(sdp) - used);
        used += (size_t)written;
    }
    assert(agent_set_remote_description(&agent, sdp) != 0);
    assert(agent.remote_candidates_count == 0);
}

static void test_pair_capacity_updates_are_transactional(void) {
    h2_pal_net_addr_t source;
    memset(&source, 0, sizeof(source));
    source.family = H2_PAL_NET_FAMILY_IPV4;
    source.ip[0] = 127u;
    source.ip[3] = 1u;
    source.port = 4000u;

    Agent agent;
    memset(&agent, 0, sizeof(agent));
    agent.net = &test_net;
    ice_candidate_create(
        &agent.local_candidates[0], 1, ICE_CANDIDATE_TYPE_HOST, &source);
    agent.local_candidates_count = 1;
    agent.candidate_pairs_num = AGENT_MAX_CANDIDATE_PAIRS;
    agent.candidate_pairs[0].state = ICE_CANDIDATE_STATE_SUCCEEDED;
    agent.selected_pair = &agent.candidate_pairs[0];

    IceCandidate remote = agent.local_candidates[0];
    remote.addr.port = 5000u;
    agent.remote_candidates[0] = remote;
    agent.remote_candidates_count = 1;
    assert(agent_update_candidate_pairs(&agent) != 0);
    assert(agent.candidate_pairs_num == AGENT_MAX_CANDIDATE_PAIRS);
    assert(agent.candidate_pairs[0].state == ICE_CANDIDATE_STATE_SUCCEEDED);
    assert(agent.selected_pair == &agent.candidate_pairs[0]);

    agent.remote_candidates_count = 0;
    assert(agent_add_remote_candidate(&agent, &remote) != 0);
    assert(agent.remote_candidates_count == 0);
    assert(agent.candidate_pairs_num == AGENT_MAX_CANDIDATE_PAIRS);

    snprintf(agent.remote_ufrag, sizeof(agent.remote_ufrag), "old");
    snprintf(agent.remote_upwd, sizeof(agent.remote_upwd), "old-password");
    char sdp[] =
        "a=ice-ufrag:new\r\n"
        "a=ice-pwd:new-password\r\n"
        "a=candidate:1 1 UDP 123 127.0.0.1 5000 typ host\r\n";
    assert(agent_set_remote_description(&agent, sdp) != 0);
    assert(agent.remote_candidates_count == 0);
    assert(strcmp(agent.remote_ufrag, "old") == 0);
    assert(strcmp(agent.remote_upwd, "old-password") == 0);
    assert(agent.candidate_pairs_num == AGENT_MAX_CANDIDATE_PAIRS);
}

static void test_credential_terminator_bound(void) {
    Agent agent;
    memset(&agent, 0, sizeof(agent));
    agent.net = &test_net;
    char sdp[ICE_UFRAG_LENGTH + sizeof("a=ice-ufrag:\r\n")];
    int prefix_len = snprintf(sdp, sizeof(sdp), "a=ice-ufrag:");
    assert(prefix_len > 0);
    memset(sdp + prefix_len, 'u', ICE_UFRAG_LENGTH);
    memcpy(sdp + prefix_len + ICE_UFRAG_LENGTH, "\r\n", 3u);
    assert(agent_set_remote_description(&agent, sdp) == 0);
    assert(agent.remote_ufrag[ICE_UFRAG_LENGTH] == '\0');

    char oversized[ICE_UFRAG_LENGTH + sizeof("a=ice-ufrag:x\r\n")];
    prefix_len = snprintf(oversized, sizeof(oversized), "a=ice-ufrag:");
    assert(prefix_len > 0);
    memset(oversized + prefix_len, 'u', ICE_UFRAG_LENGTH + 1u);
    memcpy(oversized + prefix_len + ICE_UFRAG_LENGTH + 1u, "\r\n", 3u);
    assert(agent_set_remote_description(&agent, oversized) != 0);
}

static void test_ice_server_url_bounds(void) {
    Agent agent;
    char overlong[sizeof("stun::3478") + 64u];
    memset(&agent, 0, sizeof(agent));
    agent.net = &test_net;
    memcpy(overlong, "stun:", 5u);
    memset(overlong + 5u, 'h', 64u);
    memcpy(overlong + 69u, ":3478", 6u);

    assert(agent_gather_candidate(&agent, overlong, NULL, NULL) != 0);
    assert(agent_gather_candidate(&agent, "x", NULL, NULL) != 0);
    assert(agent_gather_candidate(
               &agent, "stun:127.0.0.1:70000", NULL, NULL) != 0);
}

int main(void) {
    test_bounded_candidate_parser();
    test_remote_description_keeps_udp_and_passive_tcp();
    test_local_tcp_format_and_pair_order();
    test_trickle_pair_update_preserves_active_pair();
    test_connect_failure_consumes_one_pair_per_poll();
    test_connectivity_check_uses_wall_clock_deadline();
    test_candidate_limit();
    test_pair_capacity_updates_are_transactional();
    test_credential_terminator_bound();
    test_ice_server_url_bounds();
    return 0;
}
