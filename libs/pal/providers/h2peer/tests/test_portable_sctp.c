#include "sctp.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_sctp_association {
    h2_pal_sctp_association_config_t config;
};

typedef struct fake_sctp {
    struct h2_pal_sctp_association association;
    size_t sent_count;
    uint16_t last_stream_id;
    uint32_t last_ppid;
    size_t last_len;
    bool last_unordered;
    h2_pal_sctp_reliability_t last_reliability;
    uint32_t last_reliability_value;
    h2_pal_result_t input_result;
    h2_pal_result_t send_result;
    uint16_t reset_stream_id;
    size_t reset_call_count;
    h2_pal_result_t reset_result;
} fake_sctp_t;

typedef struct message_capture {
    size_t count;
    uint16_t sid;
    int is_text;
    size_t len;
    char data[16];
    size_t reset_count;
    h2_pal_sctp_stream_reset_event_t reset_event;
    size_t local_open_count;
    uint16_t local_open_sid;
    size_t remote_count;
    SctpRemoteChannel remote;
    char remote_label[129];
    int reject_remote;
} message_capture_t;

static void *test_alloc(void *user, size_t len) {
    (void)user;
    return malloc(len);
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
    (void)user;
    free(ptr);
}

static const h2_pal_mem_vtable_t test_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static h2_pal_result_t fake_now(void *user, uint64_t *out_ms) {
    *out_ms = *(uint64_t *)user;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_sleep(void *user, uint32_t delay_ms) {
    *(uint64_t *)user += delay_ms;
    return H2_PAL_OK;
}

static const h2_pal_time_vtable_t test_time_vtable = {
    .get_monotonic_ms = fake_now,
    .sleep_ms = fake_sleep,
};

static h2_pal_result_t
fake_create(void *user, const h2_pal_sctp_association_config_t *config,
            h2_pal_sctp_association_t **out_association) {
    fake_sctp_t *fake = user;
    fake->association.config = *config;
    *out_association = &fake->association;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_start(void *user,
                                  h2_pal_sctp_association_t *association,
                                  uint64_t now_ms) {
    (void)user;
    (void)now_ms;
    association->config.callbacks.on_state(
        association->config.callbacks.user, association,
        H2_PAL_SCTP_STATE_CONNECTED, H2_PAL_OK);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_input(void *user,
                                  h2_pal_sctp_association_t *association,
                                  const uint8_t *packet, size_t packet_len,
                                  uint64_t now_ms) {
    fake_sctp_t *fake = user;
    (void)association;
    (void)packet;
    (void)packet_len;
    (void)now_ms;
    return fake->input_result;
}

static h2_pal_result_t fake_service(void *user,
                                    h2_pal_sctp_association_t *association,
                                    uint64_t now_ms,
                                    uint64_t *out_deadline_ms) {
    (void)user;
    (void)association;
    *out_deadline_ms = now_ms + 100u;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_send(void *user,
                                 h2_pal_sctp_association_t *association,
                                 const h2_pal_sctp_message_t *message,
                                 uint64_t now_ms) {
    fake_sctp_t *fake = user;
    (void)association;
    (void)now_ms;
    fake->sent_count++;
    fake->last_stream_id = message->stream_id;
    fake->last_ppid = message->ppid;
    fake->last_len = message->len;
    fake->last_unordered = message->unordered;
    fake->last_reliability = message->reliability;
    fake->last_reliability_value = message->reliability_value;
    return fake->send_result;
}

static h2_pal_result_t fake_reset(void *user,
                                  h2_pal_sctp_association_t *association,
                                  uint16_t stream_id, uint64_t now_ms) {
    (void)association;
    (void)now_ms;
    fake_sctp_t *fake = user;
    fake->reset_stream_id = stream_id;
    fake->reset_call_count++;
    return fake->reset_result;
}

static h2_pal_result_t fake_is_writable(
    void *user,
    h2_pal_sctp_association_t *association,
    bool *out_writable) {
    (void)user;
    (void)association;
    *out_writable = true;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_shutdown(void *user,
                                     h2_pal_sctp_association_t *association,
                                     uint64_t now_ms) {
    (void)user;
    (void)association;
    (void)now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_abort(void *user,
                                  h2_pal_sctp_association_t *association,
                                  h2_pal_result_t reason, uint64_t now_ms) {
    (void)user;
    (void)association;
    (void)reason;
    (void)now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_close(void *user,
                                  h2_pal_sctp_association_t **association) {
    (void)user;
    *association = NULL;
    return H2_PAL_OK;
}

static const h2_pal_sctp_vtable_t fake_sctp_vtable = {
    .association_create = fake_create,
    .association_start = fake_start,
    .association_input_packet = fake_input,
    .association_service = fake_service,
    .association_send_message = fake_send,
    .association_is_writable = fake_is_writable,
    .association_reset_stream = fake_reset,
    .association_shutdown = fake_shutdown,
    .association_abort = fake_abort,
    .association_close = fake_close,
};

static h2_pal_result_t capture_message(char *message, size_t len, void *user,
                                       uint16_t sid, int is_text) {
    message_capture_t *capture = user;
    capture->count++;
    capture->sid = sid;
    capture->is_text = is_text;
    capture->len = len;
    memcpy(capture->data, message, len);
    return H2_PAL_OK;
}

static void capture_stream_reset(const h2_pal_sctp_stream_reset_event_t *event,
                                 void *user) {
    message_capture_t *capture = user;
    capture->reset_count++;
    capture->reset_event = *event;
}

static void capture_local_channel_open(uint16_t sid, void *user) {
    message_capture_t *capture = user;
    capture->local_open_count++;
    capture->local_open_sid = sid;
}

static int capture_remote_channel(const SctpRemoteChannel *channel,
                                  void *user) {
    message_capture_t *capture = user;
    if (capture->reject_remote) {
        return -1;
    }
    capture->remote_count++;
    capture->remote = *channel;
    assert(channel->label_len < sizeof(capture->remote_label));
    memcpy(capture->remote_label, channel->label, channel->label_len);
    capture->remote_label[channel->label_len] = '\0';
    capture->remote.label = capture->remote_label;
    return 0;
}

int main(void) {
    const h2_pal_mem_api_t mem = {
        .user = NULL,
        .vtable = &test_mem_vtable,
    };
    uint64_t now_ms = 10u;
    const h2_pal_time_api_t time = {
        .user = &now_ms,
        .vtable = &test_time_vtable,
    };
    fake_sctp_t fake;
    memset(&fake, 0, sizeof(fake));
    const h2_pal_sctp_api_t api = {
        .user = &fake,
        .vtable = &fake_sctp_vtable,
    };
    message_capture_t capture;
    memset(&capture, 0, sizeof(capture));
    Sctp sctp;
    memset(&sctp, 0, sizeof(sctp));
    sctp.mem = &mem;
    sctp.api = &api;
    sctp.time = &time;
    sctp.userdata = &capture;
    sctp_onmessage(&sctp, capture_message);
    sctp_onstreamreset(&sctp, capture_stream_reset);
    sctp_onlocalchannelopen(&sctp, capture_local_channel_open);
    sctp_onremotechannel(&sctp, capture_remote_channel);

    DtlsSrtp dtls;
    memset(&dtls, 0, sizeof(dtls));
    dtls.role = DTLS_SRTP_ROLE_SERVER;
    assert(sctp_create_association(&sctp, &dtls) == 0);
    assert(sctp_is_connected(&sctp));
    assert(sctp_service(&sctp) == 0);
    char input_packet[] = {0x13u};
    fake.input_result = H2_PAL_ERR_WOULD_BLOCK;
    sctp_incoming_data(&sctp, input_packet, sizeof(input_packet));
    assert(sctp_is_connected(&sctp));
    assert(sctp_service(&sctp) == 0);
    fake.input_result = H2_PAL_OK;

    char payload[] = "hello";
    assert(sctp_outgoing_data(&sctp, payload, 5u, PPID_STRING, 6u) == 5);
    assert(fake.sent_count == 1u);
    assert(fake.last_stream_id == 6u);
    assert(fake.last_ppid == PPID_STRING);
    assert(fake.last_len == 5u);
    assert(!fake.last_unordered);
    assert(fake.last_reliability == H2_PAL_SCTP_RELIABILITY_RELIABLE);
    assert(fake.last_reliability_value == 0u);

    assert(sctp_register_data_channel(&sctp, "local", 12u, 0x00u, 0u) == 0);
    assert(sctp_register_data_channel(&sctp, "duplicate", 12u, 0x00u, 0u) ==
           -1);
    assert(sctp_outgoing_data(&sctp, payload, 5u, PPID_STRING, 12u) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(fake.sent_count == 1u);
    char dcep_ack[] = {DATA_CHANNEL_ACK};
    assert(sctp_handle_incoming_data(&sctp, dcep_ack, sizeof(dcep_ack),
                                     PPID_CONTROL, 12u, 0) == 0);
    assert(capture.local_open_count == 1u);
    assert(capture.local_open_sid == 12u);
    assert(sctp_handle_incoming_data(&sctp, dcep_ack, sizeof(dcep_ack),
                                     PPID_CONTROL, 12u, 0) == 0);
    assert(capture.local_open_count == 1u);
    assert(sctp_register_data_channel(&sctp, "padded-local", 14u, 0x00u, 0u) ==
           0);
    char padded_dcep_ack[] = {DATA_CHANNEL_ACK, 0, 0, 0};
    assert(sctp_handle_incoming_data(&sctp, padded_dcep_ack,
                                     sizeof(padded_dcep_ack), PPID_CONTROL, 14u,
                                     0) == 0);
    assert(capture.local_open_count == 2u);
    assert(capture.local_open_sid == 14u);
    assert(sctp_unregister_data_channel(&sctp, 14u) == 0);
    assert(sctp_register_data_channel(&sctp, "bad-padded-local", 26u, 0x00u,
                                      0u) == 0);
    char bad_padded_dcep_ack[] = {DATA_CHANNEL_ACK, 0, 0, 1};
    assert(sctp_handle_incoming_data(&sctp, bad_padded_dcep_ack,
                                     sizeof(bad_padded_dcep_ack), PPID_CONTROL,
                                     26u, 0) == 0);
    assert(capture.local_open_count == 2u);
    assert(fake.reset_stream_id == 26u);
    assert(sctp_outgoing_data(&sctp, payload, 5u, PPID_STRING, 12u) == 5);
    assert(fake.sent_count == 2u);

    char dcep[] = {
        DATA_CHANNEL_OPEN,
        0x81,
        0,
        0,
        0,
        0,
        0,
        3,
        0,
        4,
        0,
        0,
        't',
        'e',
        's',
        't',
    };
    assert(sctp_handle_incoming_data(&sctp, dcep, sizeof(dcep), PPID_CONTROL,
                                     8u, 0) == 0);
    assert(fake.sent_count == 3u);
    assert(fake.last_ppid == PPID_CONTROL);
    assert(fake.last_stream_id == 8u);
    assert(capture.remote_count == 1u);
    assert(capture.remote.sid == 8u);
    assert(strcmp(capture.remote.label, "test") == 0);
    assert(capture.remote.unordered);
    assert(capture.remote.reliability ==
           H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS);
    assert(capture.remote.reliability_value == 3u);
    assert(!fake.last_unordered);
    assert(fake.last_reliability == H2_PAL_SCTP_RELIABILITY_RELIABLE);
    assert(fake.last_reliability_value == 0u);

    size_t sent_count = fake.sent_count;
    assert(sctp_handle_incoming_data(&sctp, dcep, sizeof(dcep), PPID_CONTROL,
                                     8u, 0) == 0);
    assert(fake.sent_count == sent_count);
    assert(capture.remote_count == 1u);

    assert(sctp_handle_incoming_data(&sctp, dcep, sizeof(dcep), PPID_CONTROL,
                                     9u, 0) == 0);
    assert(fake.reset_stream_id == 9u);
    assert(sctp_handle_incoming_data(&sctp, dcep, sizeof(dcep), PPID_CONTROL,
                                     300u, 0) == 0);
    assert(fake.reset_stream_id == 300u);
    char bad_length_dcep[sizeof(dcep)];
    memcpy(bad_length_dcep, dcep, sizeof(dcep));
    bad_length_dcep[11] = 1;
    assert(sctp_handle_incoming_data(&sctp, bad_length_dcep,
                                     sizeof(bad_length_dcep), PPID_CONTROL, 14u,
                                     0) == 0);
    assert(fake.reset_stream_id == 14u);
    char empty_label_dcep[12] = {
        DATA_CHANNEL_OPEN, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    assert(sctp_handle_incoming_data(&sctp, empty_label_dcep,
                                     sizeof(empty_label_dcep), PPID_CONTROL,
                                     16u, 0) == 0);
    assert(fake.reset_stream_id == 16u);
    char unsupported_dcep[sizeof(dcep)];
    memcpy(unsupported_dcep, dcep, sizeof(dcep));
    unsupported_dcep[1] = 0x03;
    assert(sctp_handle_incoming_data(&sctp, unsupported_dcep,
                                     sizeof(unsupported_dcep), PPID_CONTROL,
                                     18u, 0) == 0);
    assert(fake.reset_stream_id == 18u);
    capture.reject_remote = 1;
    assert(sctp_handle_incoming_data(&sctp, dcep, sizeof(dcep), PPID_CONTROL,
                                     20u, 0) == 0);
    capture.reject_remote = 0;
    assert(fake.sent_count == sent_count + 1u);
    assert(capture.remote_count == 1u);
    assert(fake.reset_stream_id == 20u);

    fake.send_result = H2_PAL_ERR_IO;
    assert(sctp_handle_incoming_data(&sctp, dcep, sizeof(dcep), PPID_CONTROL,
                                     24u, 0) == 0);
    fake.send_result = H2_PAL_OK;
    assert(capture.remote_count == 1u);
    assert(fake.reset_stream_id == 24u);
    assert(fake.sent_count == sent_count + 2u);

    size_t message_count = capture.count;
    assert(sctp_handle_incoming_data(&sctp, payload, 5u, PPID_STRING, 22u, 0) ==
           0);
    assert(capture.count == message_count);

    assert(sctp_outgoing_data(&sctp, payload, 5u, PPID_BINARY, 8u) == 5);
    assert(fake.sent_count == sent_count + 3u);
    assert(fake.last_unordered);
    assert(fake.last_reliability == H2_PAL_SCTP_RELIABILITY_MAX_RETRANSMITS);
    assert(fake.last_reliability_value == 3u);

    char zero_lifetime_dcep[] = {
        DATA_CHANNEL_OPEN,
        0x82,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        4,
        0,
        0,
        'z',
        'e',
        'r',
        'o',
    };
    assert(sctp_handle_incoming_data(&sctp, zero_lifetime_dcep,
                                     sizeof(zero_lifetime_dcep), PPID_CONTROL,
                                     10u, 0) == 0);
    assert(fake.sent_count == sent_count + 4u);
    assert(capture.remote_count == 2u);
    assert(sctp_outgoing_data(&sctp, payload, 5u, PPID_BINARY, 10u) == 5);
    assert(fake.sent_count == sent_count + 5u);
    assert(fake.last_unordered);
    assert(fake.last_reliability == H2_PAL_SCTP_RELIABILITY_MAX_LIFETIME_MS);
    assert(fake.last_reliability_value == 0u);

    assert(sctp_handle_incoming_data(&sctp, payload, 5u, PPID_STRING, 8u, 0) ==
           0);
    assert(capture.count == 1u);
    assert(capture.sid == 8u);
    assert(capture.is_text);
    assert(capture.len == 5u);
    assert(memcmp(capture.data, payload, 5u) == 0);

    char protocol_dcep[] = {
        DATA_CHANNEL_OPEN, 0x00, 0,   0,   0,   0,   0,   0,   0, 5, 0, 6,
        'a',               'u',  'd', 'i', 'o', 'b', 'i', 'n', 'a', 'r', 'y',
    };
    assert(sctp_handle_incoming_data(&sctp, protocol_dcep,
                                     sizeof(protocol_dcep), PPID_CONTROL, 28u,
                                     0) == 0);
    assert(capture.remote_count == 3u);
    assert(capture.remote.sid == 28u);
    assert(strcmp(capture.remote.label, "audio") == 0);

    assert(sctp_close_stream(&sctp, 8u) == H2_PAL_OK);
    assert(fake.reset_stream_id == 8u);
    fake.reset_result = H2_PAL_ERR_BUSY;
    assert(sctp_close_stream(&sctp, 10u) == H2_PAL_ERR_BUSY);
    h2_pal_sctp_stream_reset_event_t reset_event = {
        .stream_id = 8u,
        .direction = H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET,
        .result = H2_PAL_OK,
    };
    fake.association.config.callbacks.on_stream_reset(
        fake.association.config.callbacks.user, &fake.association,
        &reset_event);
    assert(capture.reset_count == 1u);
    assert(capture.reset_event.stream_id == 8u);
    assert(capture.reset_event.direction ==
           H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET);
    size_t stream_count = sctp.stream_count;
    assert(sctp_unregister_data_channel(&sctp, 8u) == 0);
    assert(sctp.stream_count + 1u == stream_count);
    assert(sctp_unregister_data_channel(&sctp, 8u) == -1);
    sctp_destroy_association(&sctp);
    assert(sctp.association == NULL);
    assert(sctp.stream_table == NULL);
    return 0;
}
