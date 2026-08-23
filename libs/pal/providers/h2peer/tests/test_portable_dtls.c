#include "dtls_srtp.h"
#include "peer.h"
#include "peer_connection.h"
#include "rtp.h"
#include "h2_wolfcrypt_crypto.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct h2_pal_dtls_session {
    h2_pal_dtls_session_config_t config;
    uint8_t remote_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    int remote_fingerprint_set;
    int closed;
};

typedef struct fake_dtls {
    struct h2_pal_dtls_session session;
    uint8_t local_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    uint64_t now_ms;
    size_t create_count;
    size_t destroy_count;
    size_t flush_count;
    h2_pal_result_t write_result;
    h2_pal_result_t flush_result;
    h2_pal_result_t consume_result;
    h2_pal_result_t handshake_result;
} fake_dtls_t;

static int test_log_write(void *user, h2_pal_log_level_t level,
                          const char *scope, const char *message) {
  (void)user;
  (void)level;
  assert(scope != NULL);
  assert(message != NULL);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_create(void *user,
                                   const h2_pal_dtls_session_config_t *config,
                                   h2_pal_dtls_session_t **out_session) {
  fake_dtls_t *fake = user;
  memset(&fake->session, 0, sizeof(fake->session));
  fake->session.config = *config;
  fake->create_count++;
  *out_session = &fake->session;
  return H2_PAL_OK;
}

static h2_pal_result_t fake_local_fingerprint(
    void *user,
    h2_pal_dtls_session_t *session,
    uint8_t out[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    (void)session;
    memcpy(out, ((fake_dtls_t *)user)->local_fingerprint,
           H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE);
    return H2_PAL_OK;
}

static h2_pal_result_t fake_remote_fingerprint(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    (void)user;
    memcpy(session->remote_fingerprint, fingerprint,
           H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE);
    session->remote_fingerprint_set = 1;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_handshake(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len,
    uint64_t now_ms,
    uint64_t deadline_ms,
    int *out_complete) {
    fake_dtls_t *fake = user;
    if (fake->handshake_result != H2_PAL_OK) {
        return fake->handshake_result;
    }
    if (!session->remote_fingerprint_set) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (now_ms >= deadline_ms) {
        return H2_PAL_ERR_TIMEOUT;
    }
    if (datagram_len == 1u && datagram[0] == 0x16u) {
        *out_complete = 1;
        return H2_PAL_OK;
    }
    return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t fake_next_deadline(
    void *user,
    h2_pal_dtls_session_t *session,
    uint64_t *out_deadline_ms) {
    (void)session;
    *out_deadline_ms = ((fake_dtls_t *)user)->now_ms + 100u;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_flush(
    void *user, h2_pal_dtls_session_t *session) {
    (void)session;
    fake_dtls_t *fake = user;
    fake->flush_count++;
    return fake->flush_result;
}

static h2_pal_result_t fake_profile(
    void *user,
    h2_pal_dtls_session_t *session,
    h2_pal_dtls_srtp_profile_t *out_profile) {
    (void)user;
    (void)session;
    *out_profile = H2_PAL_DTLS_SRTP_PROFILE_AES128_CM_SHA1_80;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_export(
    void *user,
    h2_pal_dtls_session_t *session,
    uint8_t *out,
    size_t out_len) {
    (void)user;
    (void)session;
    for (size_t i = 0u; i < out_len; ++i) {
        out[i] = (uint8_t)i;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t fake_write(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t *data,
    size_t data_len) {
    fake_dtls_t *fake = user;
    if (fake->write_result != H2_PAL_OK) {
        return fake->write_result;
    }
    return session->config.send(
        session->config.io_user, data, data_len);
}

static h2_pal_result_t fake_consume(
    void *user,
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len) {
    fake_dtls_t *fake = user;
    if (fake->consume_result != H2_PAL_OK) {
        return fake->consume_result;
    }
    return session->config.plaintext(
        session->config.io_user, datagram, datagram_len);
}

static h2_pal_result_t fake_close(
    void *user, h2_pal_dtls_session_t *session) {
    (void)user;
    session->closed = 1;
    return H2_PAL_OK;
}

static void fake_destroy(
    void *user, h2_pal_dtls_session_t **session) {
    fake_dtls_t *fake = user;
    assert((*session)->closed);
    fake->destroy_count++;
    *session = NULL;
}

static const h2_pal_dtls_vtable_t fake_dtls_vtable = {
    .session_create = fake_create,
    .session_get_local_fingerprint = fake_local_fingerprint,
    .session_set_remote_fingerprint = fake_remote_fingerprint,
    .session_handshake = fake_handshake,
    .session_next_deadline_ms = fake_next_deadline,
    .session_flush = fake_flush,
    .session_get_srtp_profile = fake_profile,
    .session_export_srtp_keying_material = fake_export,
    .session_write = fake_write,
    .session_consume_datagram = fake_consume,
    .session_close = fake_close,
    .session_destroy = fake_destroy,
};

static h2_pal_result_t fake_monotonic(void *user, uint64_t *out_ms) {
    *out_ms = ((fake_dtls_t *)user)->now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t fake_sleep(void *user, uint32_t delay_ms) {
    ((fake_dtls_t *)user)->now_ms += delay_ms;
    return H2_PAL_OK;
}

static const h2_pal_time_vtable_t fake_time_vtable = {
    .get_monotonic_ms = fake_monotonic,
    .sleep_ms = fake_sleep,
};

static void *test_alloc(void *user, size_t size) {
    (void)user;
    return malloc(size);
}

static void *test_realloc(void *user, void *ptr, size_t size) {
    (void)user;
    return realloc(ptr, size);
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

static int test_entropy(void *user, uint8_t *out, size_t len) {
    uint8_t *counter = user;
    for (size_t i = 0u; i < len; ++i) {
        out[i] = (*counter)++;
    }
    return H2_PAL_OK;
}

static int capture_send(
    void *user, const unsigned char *data, size_t len) {
    DtlsSrtp *session = user;
    size_t *captured = session->user_data;
    (void)data;
    *captured = len;
    return (int)len;
}

typedef struct fake_rtp_transport {
    int protect_calls;
    int send_calls;
    int send_results[4];
    uint8_t sent_packets[4][32];
    size_t sent_lens[4];
    uint8_t pending_packet[32];
    size_t pending_len;
} fake_rtp_transport_t;

static int fake_rtp_protect(void *user, uint8_t *packet, int *packet_len) {
    fake_rtp_transport_t *fake = user;
    fake->protect_calls++;
    for (int index = 0; index < *packet_len; ++index) {
        packet[index] ^= 0x5au;
    }
    return 0;
}

static int fake_rtp_send(void *user, const uint8_t *packet, int packet_len) {
    fake_rtp_transport_t *fake = user;
    assert(fake->send_calls < 4);
    assert(packet_len >= 0 && (size_t)packet_len <= 32u);
    const int index = fake->send_calls++;
    memcpy(fake->sent_packets[index], packet, (size_t)packet_len);
    fake->sent_lens[index] = (size_t)packet_len;
    return fake->send_results[index];
}

static int fake_rtp_encoder_send(uint8_t *packet, size_t packet_len,
                                 void *user) {
    fake_rtp_transport_t *fake = user;
    return peer_connection_rtp_send_or_queue(
        fake->pending_packet, sizeof(fake->pending_packet), &fake->pending_len,
        packet, packet_len, fake_rtp_protect, fake, fake_rtp_send, fake);
}

static void test_protected_rtp_backpressure(void) {
    fake_rtp_transport_t fake;
    memset(&fake, 0, sizeof(fake));
    fake.send_results[0] = H2_PAL_ERR_WOULD_BLOCK;
    fake.send_results[1] = H2_PAL_ERR_WOULD_BLOCK;
    fake.send_results[2] = H2_PAL_OK;
    fake.send_results[3] = H2_PAL_OK;

    RtpEncoder encoder;
    rtp_encoder_init(&encoder, CODEC_OPUS, fake_rtp_encoder_send, &fake);
    const uint8_t first_frame[] = {1u, 2u, 3u};
    assert(rtp_encoder_encode(&encoder, first_frame, sizeof(first_frame)) == 0);
    assert(fake.protect_calls == 1);
    assert(fake.send_calls == 1);
    assert(fake.pending_len == fake.sent_lens[0]);
    assert(memcmp(fake.pending_packet, fake.sent_packets[0],
                  fake.pending_len) == 0);
    assert(encoder.seq_number == 1u);

    const uint8_t second_frame[] = {4u, 5u, 6u};
    assert(rtp_encoder_encode(&encoder, second_frame, sizeof(second_frame)) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(fake.protect_calls == 1);
    assert(fake.send_calls == 1);
    assert(encoder.seq_number == 1u);

    assert(peer_connection_rtp_flush(fake.pending_packet, &fake.pending_len,
                                     fake_rtp_send, &fake) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(fake.pending_len != 0u);
    assert(fake.send_calls == 2);
    assert(fake.sent_lens[1] == fake.sent_lens[0]);
    assert(memcmp(fake.sent_packets[0], fake.sent_packets[1],
                  fake.sent_lens[0]) == 0);
    assert(peer_connection_rtp_flush(fake.pending_packet, &fake.pending_len,
                                     fake_rtp_send, &fake) == H2_PAL_OK);
    assert(fake.pending_len == 0u);
    assert(fake.send_calls == 3);
    assert(fake.protect_calls == 1);
    assert(fake.sent_lens[2] == fake.sent_lens[0]);
    assert(memcmp(fake.sent_packets[0], fake.sent_packets[2],
                  fake.sent_lens[0]) == 0);

    assert(rtp_encoder_encode(&encoder, second_frame, sizeof(second_frame)) ==
           0);
    assert(fake.protect_calls == 2);
    assert(fake.send_calls == 4);
    assert(encoder.seq_number == 2u);
}

int main(void) {
    test_protected_rtp_backpressure();
    PeerConnectionState terminal_state = PEER_CONNECTION_CONNECTED;
    assert(peer_connection_classify_dtls_handshake_result(
               -1, &terminal_state) == H2_PAL_ERR_IO);
    assert(terminal_state == PEER_CONNECTION_FAILED);
    terminal_state = PEER_CONNECTION_CONNECTED;
    assert(peer_connection_classify_dtls_handshake_result(
               1, &terminal_state) == H2_PAL_OK);
    assert(terminal_state == PEER_CONNECTION_CONNECTED);
    assert(peer_connection_state_poll_result(PEER_CONNECTION_NEW) ==
           H2_PAL_OK);
    assert(peer_connection_state_poll_result(PEER_CONNECTION_COMPLETED) ==
           H2_PAL_OK);
    assert(peer_connection_state_poll_result(PEER_CONNECTION_FAILED) ==
           H2_PAL_ERR_IO);
    assert(peer_connection_state_poll_result(PEER_CONNECTION_DISCONNECTED) ==
           H2_PAL_ERR_CLOSED);
    assert(peer_connection_state_poll_result(PEER_CONNECTION_CLOSED) ==
           H2_PAL_ERR_CLOSED);

    fake_dtls_t fake;
    memset(&fake, 0, sizeof(fake));
    for (size_t i = 0u; i < sizeof(fake.local_fingerprint); ++i) {
        fake.local_fingerprint[i] = (uint8_t)i;
    }
    const h2_pal_dtls_api_t dtls = {
        .user = &fake,
        .vtable = &fake_dtls_vtable,
    };
    const h2_pal_time_api_t time = {
        .user = &fake,
        .vtable = &fake_time_vtable,
    };
    const h2_pal_mem_api_t mem = {
        .user = NULL,
        .vtable = &test_mem_vtable,
    };
    const h2_pal_log_vtable_t log_vtable = {
        .write = test_log_write,
    };
    const h2_pal_log_api_t log = {
        .vtable = &log_vtable,
    };
    uint8_t entropy_counter = 1u;
    const h2_wolfcrypt_crypto_config_t crypto_config = {
        .entropy_user = &entropy_counter,
        .entropy = test_entropy,
    };
    h2_wolfcrypt_crypto_deinit();
    assert(h2_wolfcrypt_crypto_init(&crypto_config) == H2_PAL_OK);
    assert(peer_init(&mem, h2_wolfcrypt_crypto_api()) == 0);

    DtlsSrtp session;
    memset(&session, 0, sizeof(session));
    size_t sent = 0u;
    assert(dtls_srtp_init(&session, DTLS_SRTP_ROLE_CLIENT, &sent, &log, &dtls,
                          &time) == 0);
    session.packet_send = capture_send;
    assert(strcmp(session.local_fingerprint,
                  "00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:"
                  "10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F") == 0);
    assert(dtls_srtp_set_remote_fingerprint(
               &session, session.local_fingerprint) == 0);
    fake.handshake_result = H2_PAL_ERR_TLS_VERIFY;
    assert(dtls_srtp_handshake(&session, NULL, 0u) == -1);
    fake.handshake_result = H2_PAL_OK;
    assert(dtls_srtp_handshake(&session, NULL, 0u) == 1);
    const uint8_t handshake[] = {0x16u};
    assert(dtls_srtp_handshake(
               &session, handshake, sizeof(handshake)) == 0);

    const uint8_t message[] = {1u, 2u, 3u};
    assert(dtls_srtp_write(&session, message, sizeof(message)) == H2_PAL_OK);
    assert(sent == sizeof(message));
    fake.write_result = H2_PAL_ERR_WOULD_BLOCK;
    sent = 0u;
    assert(dtls_srtp_write(&session, message, sizeof(message)) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(sent == 0u);
    fake.write_result = H2_PAL_OK;
    fake.flush_result = H2_PAL_ERR_WOULD_BLOCK;
    size_t flush_count = fake.flush_count;
    assert(dtls_srtp_write(&session, message, sizeof(message)) == H2_PAL_OK);
    assert(sent == sizeof(message));
    assert(fake.flush_count == flush_count + 1u);
    assert(session.output_pending);
    assert(dtls_srtp_flush_pending(&session) == H2_PAL_ERR_WOULD_BLOCK);
    assert(session.output_pending);
    fake.flush_result = H2_PAL_OK;
    assert(dtls_srtp_flush_pending(&session) == H2_PAL_OK);
    assert(!session.output_pending);
    assert(dtls_srtp_flush_pending(&session) == H2_PAL_OK);
    assert(fake.flush_count == flush_count + 3u);
    uint8_t plaintext[8] = {0};
    assert(dtls_srtp_read(
               &session, message, sizeof(message),
               plaintext, sizeof(plaintext)) == (int)sizeof(message));
    assert(memcmp(plaintext, message, sizeof(message)) == 0);
    fake.consume_result = H2_PAL_ERR_CLOSED;
    assert(dtls_srtp_read(&session, message, sizeof(message), plaintext,
                          sizeof(plaintext)) == H2_PAL_ERR_CLOSED);

    dtls_srtp_deinit(&session);
    dtls_srtp_deinit(&session);
    assert(fake.create_count == 1u);
    assert(fake.destroy_count == 1u);
    peer_deinit();
    h2_wolfcrypt_crypto_deinit();
    return 0;
}
