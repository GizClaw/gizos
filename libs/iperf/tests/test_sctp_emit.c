#include "h2_iperf_internal.h"
#include "h2_iperf_test_support.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>

typedef struct emit_fixture {
    h2_pal_sctp_callbacks_t callbacks;
    int result;
    bool partial;
} emit_fixture_t;

static int send_packet(void *user, h2_pal_net_socket_t socket,
                       const h2_pal_net_addr_t *addr, const uint8_t *data,
                       size_t len) {
    emit_fixture_t *fixture = user;
    (void)socket;
    (void)addr;
    (void)data;
    return fixture->result != 0 ? fixture->result
         : (int)(fixture->partial ? len - 1u : len);
}

static h2_pal_result_t create_association(
    void *user, const h2_pal_sctp_association_config_t *config,
    h2_pal_sctp_association_t **out) {
    emit_fixture_t *fixture = user;
    fixture->callbacks = config->callbacks;
    *out = (h2_pal_sctp_association_t *)fixture;
    return H2_PAL_OK;
}

static h2_pal_result_t close_association(
    void *user, h2_pal_sctp_association_t **association) {
    (void)user;
    *association = NULL;
    return H2_PAL_OK;
}

int main(void) {
    emit_fixture_t fixture = {0};
    const h2_pal_net_vtable_t net_vtable = {.udp_sendto = send_packet};
    const h2_pal_net_api_t net = {.user = &fixture, .vtable = &net_vtable};
    const h2_pal_sctp_vtable_t sctp_vtable = {
        .association_create = create_association,
        .association_close = close_association,
    };
    const h2_pal_sctp_api_t sctp = {.user = &fixture, .vtable = &sctp_vtable};
    h2_iperf_test_env_t env;
    h2_iperf_test_env_init(&env, false);
    env.config.net = &net;
    env.config.sctp = &sctp;
    env.config.log = NULL;
    h2_iperf_stream_t stream;
    h2_iperf_stream_init(&stream, &env.config, H2_IPERF_PROTOCOL_SCTP, NULL);
    stream.peer_known = true;
    stream.sock = 0;
    assert(h2_iperf_stream_sctp_create(&stream, H2_PAL_SCTP_ROLE_ACTIVE,
                                      5000u, 5001u, 1200u, 8192u) == H2_PAL_OK);
    const uint8_t packet[32] = {0};
    fixture.result = H2_PAL_ERR_WOULD_BLOCK;
    /* Call the production callback captured by the actual stream creator. */
    for (unsigned i = 0u; i < 2000u; ++i)
        assert(fixture.callbacks.emit_packet(fixture.callbacks.user,
                   stream.assoc, packet, sizeof(packet)) == H2_PAL_ERR_WOULD_BLOCK);
    stream.emit_failures = UINT_MAX;
    assert(fixture.callbacks.emit_packet(fixture.callbacks.user, stream.assoc,
                                         packet, sizeof(packet)) == H2_PAL_ERR_WOULD_BLOCK);
    assert(stream.emit_failures == UINT_MAX);
    fixture.result = 0;
    assert(fixture.callbacks.emit_packet(fixture.callbacks.user, stream.assoc,
                                         packet, sizeof(packet)) == H2_PAL_OK);
    assert(stream.emit_failures == 0u);
    fixture.result = H2_PAL_ERR_IO;
    assert(fixture.callbacks.emit_packet(fixture.callbacks.user, stream.assoc,
                                         packet, sizeof(packet)) == H2_PAL_ERR_IO);
    fixture.result = H2_PAL_ERR_INVALID_ARG;
    assert(fixture.callbacks.emit_packet(fixture.callbacks.user, stream.assoc,
                                         packet, sizeof(packet)) == H2_PAL_ERR_INVALID_ARG);
    fixture.result = 0;
    fixture.partial = true;
    assert(fixture.callbacks.emit_packet(fixture.callbacks.user, stream.assoc,
                                         packet, sizeof(packet)) == H2_PAL_ERR_IO);
    h2_iperf_stream_close(&stream);
    h2_iperf_test_env_deinit(&env);
    return 0;
}
