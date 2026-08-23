#include "h2_pal.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct sctp_fixture {
    unsigned calls;
    uint64_t now_ms;
    h2_pal_sctp_message_t message;
} sctp_fixture_t;

static h2_pal_result_t fixture_emit(
    void *user,
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len) {
    (void)user;
    (void)association;
    (void)packet;
    (void)packet_len;
    return H2_PAL_OK;
}

static void fixture_state(
    void *user,
    h2_pal_sctp_association_t *association,
    h2_pal_sctp_state_t state,
    h2_pal_result_t reason) {
    (void)user;
    (void)association;
    (void)state;
    (void)reason;
}

static h2_pal_result_t fixture_message(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_received_message_t *message) {
    (void)user;
    (void)association;
    (void)message;
    return H2_PAL_OK;
}

static void fixture_reset(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_stream_reset_event_t *event) {
    (void)user;
    (void)association;
    (void)event;
}

static h2_pal_sctp_association_config_t fixture_config(void) {
    const h2_pal_sctp_association_config_t config = {
        .role = H2_PAL_SCTP_ROLE_ACTIVE,
        .local_port = 5000u,
        .remote_port = 5001u,
        .inbound_streams = 8u,
        .outbound_streams = 8u,
        .max_packet_size = 1200u,
        .max_message_size = 4096u,
        .send_buffer_size = 8192u,
        .receive_buffer_size = 8192u,
        .cookie_lifetime_ms = 60000u,
        .callbacks = {
            .user = NULL,
            .emit_packet = fixture_emit,
            .on_state = fixture_state,
            .on_message = fixture_message,
            .on_stream_reset = fixture_reset,
        },
    };
    return config;
}

static h2_pal_result_t fixture_create(
    void *user,
    const h2_pal_sctp_association_config_t *config,
    h2_pal_sctp_association_t **out_association) {
    sctp_fixture_t *fixture = (sctp_fixture_t *)user;
    assert(config != NULL);
    fixture->calls++;
    *out_association = (h2_pal_sctp_association_t *)(uintptr_t)1u;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_start(
    void *user,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    sctp_fixture_t *fixture = (sctp_fixture_t *)user;
    assert(association != NULL);
    fixture->calls++;
    fixture->now_ms = now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_input(
    void *user,
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len,
    uint64_t now_ms) {
    sctp_fixture_t *fixture = (sctp_fixture_t *)user;
    assert(association != NULL && packet != NULL && packet_len >= 12u);
    fixture->calls++;
    fixture->now_ms = now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_service(
    void *user,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *out_next_deadline_ms) {
    sctp_fixture_t *fixture = (sctp_fixture_t *)user;
    assert(association != NULL);
    fixture->calls++;
    fixture->now_ms = now_ms;
    *out_next_deadline_ms = now_ms + 20u;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_send(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_message_t *message,
    uint64_t now_ms) {
    sctp_fixture_t *fixture = (sctp_fixture_t *)user;
    assert(association != NULL);
    fixture->calls++;
    fixture->now_ms = now_ms;
    fixture->message = *message;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_stream(
    void *user,
    h2_pal_sctp_association_t *association,
    uint16_t stream_id,
    uint64_t now_ms) {
    sctp_fixture_t *fixture = (sctp_fixture_t *)user;
    assert(association != NULL && stream_id == 2u);
    fixture->calls++;
    fixture->now_ms = now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_is_writable(
    void *user,
    h2_pal_sctp_association_t *association,
    bool *out_writable) {
    sctp_fixture_t *fixture = (sctp_fixture_t *)user;
    assert(association != NULL);
    fixture->calls++;
    *out_writable = true;
    return H2_PAL_OK;
}

static h2_pal_result_t fixture_shutdown(
    void *user,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    return fixture_start(user, association, now_ms);
}

static h2_pal_result_t fixture_abort(
    void *user,
    h2_pal_sctp_association_t *association,
    h2_pal_result_t reason,
    uint64_t now_ms) {
    assert(reason != H2_PAL_OK);
    return fixture_start(user, association, now_ms);
}

static h2_pal_result_t fixture_close(
    void *user,
    h2_pal_sctp_association_t **association) {
    sctp_fixture_t *fixture = (sctp_fixture_t *)user;
    fixture->calls++;
    *association = NULL;
    return H2_PAL_OK;
}

static const h2_pal_sctp_vtable_t fixture_vtable = {
    .association_create = fixture_create,
    .association_start = fixture_start,
    .association_input_packet = fixture_input,
    .association_service = fixture_service,
    .association_send_message = fixture_send,
    .association_is_writable = fixture_is_writable,
    .association_reset_stream = fixture_stream,
    .association_shutdown = fixture_shutdown,
    .association_abort = fixture_abort,
    .association_close = fixture_close,
};

int main(void) {
    sctp_fixture_t fixture = {0};
    const h2_pal_sctp_api_t api = {
        .user = &fixture,
        .vtable = &fixture_vtable,
    };
    h2_pal_sctp_association_config_t config = fixture_config();
    h2_pal_sctp_association_t *association =
        (h2_pal_sctp_association_t *)(uintptr_t)2u;

    assert(h2_pal_sctp_association_create(NULL, &config, &association) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(association == NULL);
    config.local_port = 0u;
    assert(h2_pal_sctp_association_create(&api, &config, &association) ==
           H2_PAL_ERR_INVALID_ARG);
    config = fixture_config();
    config.max_packet_size = (size_t)UINT16_MAX + 1u;
    assert(h2_pal_sctp_association_create(&api, &config, &association) ==
           H2_PAL_ERR_INVALID_ARG);
    config = fixture_config();
    assert(h2_pal_sctp_association_create(&api, &config, &association) ==
           H2_PAL_OK);
    bool writable = false;
    assert(h2_pal_sctp_association_is_writable(&api, association, &writable) ==
           H2_PAL_OK);
    assert(writable);
    assert(association != NULL && fixture.calls == 2u);

    assert(h2_pal_sctp_association_start(&api, association, 10u) == H2_PAL_OK);
    assert(fixture.now_ms == 10u);
    assert(h2_pal_sctp_association_start(
               &api, association, H2_PAL_SCTP_NO_DEADLINE) ==
           H2_PAL_ERR_INVALID_ARG);

    const uint8_t packet[12] = {0};
    assert(h2_pal_sctp_association_input_packet(
               &api, association, packet, sizeof(packet), 11u) == H2_PAL_OK);
    assert(h2_pal_sctp_association_input_packet(
               &api, association, packet, sizeof(packet) - 1u, 11u) ==
           H2_PAL_ERR_INVALID_ARG);

    uint64_t deadline = 0u;
    assert(h2_pal_sctp_association_service(
               NULL, association, 12u, &deadline) == H2_PAL_ERR_UNSUPPORTED);
    assert(deadline == H2_PAL_SCTP_NO_DEADLINE);
    assert(h2_pal_sctp_association_service(
               &api, association, 12u, &deadline) == H2_PAL_OK);
    assert(deadline == 32u);

    const uint8_t payload[] = {1u, 2u};
    h2_pal_sctp_message_t message = {
        .data = payload,
        .len = sizeof(payload),
        .stream_id = 2u,
        .ppid = 53u,
        .unordered = true,
        .reliability = H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS,
        .reliability_value = 0u,
    };
    assert(h2_pal_sctp_association_send_message(
               &api, association, &message, 13u) == H2_PAL_OK);
    assert(fixture.message.ppid == 53u && fixture.message.unordered);
    message.reliability = H2_PAL_SCTP_RELIABILITY_MAX_LIFETIME_MS;
    assert(h2_pal_sctp_association_send_message(
               &api, association, &message, 13u) == H2_PAL_OK);
    message.reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE;
    message.reliability_value = 1u;
    assert(h2_pal_sctp_association_send_message(
               &api, association, &message, 13u) == H2_PAL_ERR_INVALID_ARG);

    assert(h2_pal_sctp_association_reset_stream(
               &api, association, 2u, 14u) == H2_PAL_OK);
    assert(h2_pal_sctp_association_shutdown(&api, association, 15u) ==
           H2_PAL_OK);
    assert(h2_pal_sctp_association_abort(
               &api, association, H2_PAL_OK, 16u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_sctp_association_abort(
               &api, association, H2_PAL_ERR_IO, 16u) == H2_PAL_OK);
    assert(h2_pal_sctp_association_close(&api, &association) == H2_PAL_OK);
    assert(association == NULL);
    assert(h2_pal_sctp_association_close(NULL, &association) == H2_PAL_OK);

    const h2_pal_sctp_api_t *unsupported = h2_pal_unsupported_sctp_api();
    config = fixture_config();
    message.reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE;
    message.reliability_value = 0u;
    association = (h2_pal_sctp_association_t *)(uintptr_t)2u;
    assert(h2_pal_sctp_association_create(
               unsupported, &config, &association) == H2_PAL_ERR_UNSUPPORTED);
    assert(association == NULL);
    association = (h2_pal_sctp_association_t *)(uintptr_t)2u;
    assert(h2_pal_sctp_association_start(unsupported, association, 20u) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_sctp_association_input_packet(
               unsupported,
               association,
               packet,
               sizeof(packet),
               20u) == H2_PAL_ERR_UNSUPPORTED);
    deadline = 0u;
    assert(h2_pal_sctp_association_service(
               unsupported, association, 20u, &deadline) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(deadline == H2_PAL_SCTP_NO_DEADLINE);
    assert(h2_pal_sctp_association_send_message(
               unsupported, association, &message, 20u) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_sctp_association_reset_stream(
               unsupported, association, 2u, 20u) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_sctp_association_shutdown(
               unsupported, association, 20u) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_sctp_association_abort(
               unsupported,
               association,
               H2_PAL_ERR_IO,
               20u) == H2_PAL_ERR_UNSUPPORTED);
    assert(h2_pal_sctp_association_close(unsupported, &association) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(association != NULL);
    association = NULL;
    assert(h2_pal_sctp_association_close(unsupported, &association) ==
           H2_PAL_OK);

    return 0;
}
