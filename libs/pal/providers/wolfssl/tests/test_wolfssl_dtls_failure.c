#include "h2_wolfssl.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PACKET_CAPACITY 64u
#define TEST_PACKET_SIZE 1500u
#define TEST_PENDING_CAPACITY 16384u
#define TEST_HANDSHAKE_DEADLINE_MS 60000u

typedef struct test_packet {
    uint8_t bytes[TEST_PACKET_SIZE];
    size_t len;
} test_packet_t;

typedef struct test_queue {
    test_packet_t packets[TEST_PACKET_CAPACITY];
    size_t head;
    size_t count;
} test_queue_t;

typedef struct test_endpoint {
    test_queue_t *outgoing;
    h2_pal_result_t send_result;
    h2_pal_result_t plaintext_result;
} test_endpoint_t;

typedef struct test_alloc_state {
    size_t calls;
    size_t fail_at;
    size_t live;
} test_alloc_state_t;

typedef struct test_entropy_state {
    uint32_t value;
    int fail;
} test_entropy_state_t;

static void *test_alloc(void *user, size_t len) {
    test_alloc_state_t *state = user;
    void *ptr;
    ++state->calls;
    if (state->fail_at != 0u && state->calls == state->fail_at) {
        return NULL;
    }
    ptr = malloc(len);
    if (ptr != NULL) {
        ++state->live;
    }
    return ptr;
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    test_alloc_state_t *state = user;
    void *result;
    ++state->calls;
    if (state->fail_at != 0u && state->calls == state->fail_at) {
        return NULL;
    }
    result = realloc(ptr, len);
    if (result != NULL && ptr == NULL) {
        ++state->live;
    }
    return result;
}

static void test_free(void *user, void *ptr) {
    test_alloc_state_t *state = user;
    if (ptr != NULL) {
        assert(state->live > 0u);
        --state->live;
    }
    free(ptr);
}

static int test_entropy(void *user, uint8_t *out, size_t len) {
    test_entropy_state_t *state = user;
    size_t index;
    if (state->fail) {
        return H2_PAL_ERR_TIMEOUT;
    }
    for (index = 0u; index < len; ++index) {
        state->value = state->value * 1664525u + 1013904223u;
        out[index] = (uint8_t)(state->value >> 24u);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t queue_send(
    void *user, const uint8_t *data, size_t len) {
    test_endpoint_t *endpoint = user;
    size_t tail;
    if (endpoint->send_result != H2_PAL_OK) {
        return endpoint->send_result;
    }
    assert(len <= TEST_PACKET_SIZE);
    assert(endpoint->outgoing->count < TEST_PACKET_CAPACITY);
    tail = (endpoint->outgoing->head + endpoint->outgoing->count) %
           TEST_PACKET_CAPACITY;
    memcpy(endpoint->outgoing->packets[tail].bytes, data, len);
    endpoint->outgoing->packets[tail].len = len;
    ++endpoint->outgoing->count;
    return H2_PAL_OK;
}

static h2_pal_result_t collect_plaintext(
    void *user, const uint8_t *data, size_t len) {
    test_endpoint_t *endpoint = user;
    (void)data;
    (void)len;
    return endpoint->plaintext_result;
}

static int queue_pop(test_queue_t *queue, test_packet_t *out) {
    if (queue->count == 0u) {
        return 0;
    }
    *out = queue->packets[queue->head];
    queue->head = (queue->head + 1u) % TEST_PACKET_CAPACITY;
    --queue->count;
    return 1;
}

static h2_pal_dtls_session_t *create_session(
    h2_pal_dtls_role_t role,
    test_endpoint_t *endpoint,
    size_t pending_capacity) {
    h2_pal_dtls_session_config_t config = {
        .role = role,
        .max_datagram_size = TEST_PACKET_SIZE,
        .max_plaintext_size = 1024u,
        .max_pending_output_bytes = pending_capacity,
        .send = queue_send,
        .plaintext = collect_plaintext,
        .io_user = endpoint,
    };
    h2_pal_dtls_session_t *session = NULL;
    assert(h2_pal_dtls_session_create(
               h2_wolfssl_dtls_api(), &config, &session) == H2_PAL_OK);
    return session;
}

static void configure_pair(
    h2_pal_dtls_session_t *client,
    h2_pal_dtls_session_t *server,
    int mismatch_client_fingerprint) {
    uint8_t client_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    uint8_t server_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];

    assert(h2_pal_dtls_session_get_local_fingerprint(
               h2_wolfssl_dtls_api(), client, client_fingerprint) ==
           H2_PAL_OK);
    assert(h2_pal_dtls_session_get_local_fingerprint(
               h2_wolfssl_dtls_api(), server, server_fingerprint) ==
           H2_PAL_OK);
    if (mismatch_client_fingerprint) {
        server_fingerprint[0] ^= 0x80u;
    }
    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), client, server_fingerprint) ==
           H2_PAL_OK);
    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), server, client_fingerprint) ==
           H2_PAL_OK);
}

static h2_pal_result_t drive_pair(
    h2_pal_dtls_session_t *client,
    h2_pal_dtls_session_t *server,
    test_queue_t *client_to_server,
    test_queue_t *server_to_client) {
    test_packet_t packet;
    uint64_t now_ms = 1u;
    int client_complete = 0;
    int server_complete = 0;
    int iterations;
    h2_pal_result_t result;

    result = h2_pal_dtls_session_handshake(
        h2_wolfssl_dtls_api(), client, NULL, 0u, now_ms,
        TEST_HANDSHAKE_DEADLINE_MS, &client_complete);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return result;
    }
    for (iterations = 0; iterations < 1000 &&
                         (!client_complete || !server_complete);
         ++iterations) {
        int progressed = 0;
        while (queue_pop(client_to_server, &packet)) {
            result = h2_pal_dtls_session_handshake(
                h2_wolfssl_dtls_api(), server, packet.bytes, packet.len,
                now_ms, TEST_HANDSHAKE_DEADLINE_MS, &server_complete);
            if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
                return result;
            }
            progressed = 1;
        }
        while (queue_pop(server_to_client, &packet)) {
            result = h2_pal_dtls_session_handshake(
                h2_wolfssl_dtls_api(), client, packet.bytes, packet.len,
                now_ms, TEST_HANDSHAKE_DEADLINE_MS, &client_complete);
            if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
                return result;
            }
            progressed = 1;
        }
        if (!progressed && (!client_complete || !server_complete)) {
            uint64_t client_deadline = UINT64_MAX;
            uint64_t server_deadline = UINT64_MAX;
            if (!client_complete) {
                assert(h2_pal_dtls_session_next_deadline_ms(
                           h2_wolfssl_dtls_api(), client,
                           &client_deadline) == H2_PAL_OK);
            }
            if (!server_complete) {
                assert(h2_pal_dtls_session_next_deadline_ms(
                           h2_wolfssl_dtls_api(), server,
                           &server_deadline) == H2_PAL_OK);
            }
            now_ms = client_deadline < server_deadline
                         ? client_deadline
                         : server_deadline;
            if (!client_complete && now_ms == client_deadline) {
                result = h2_pal_dtls_session_handshake(
                    h2_wolfssl_dtls_api(), client, NULL, 0u, now_ms,
                    TEST_HANDSHAKE_DEADLINE_MS, &client_complete);
                if (result != H2_PAL_OK &&
                    result != H2_PAL_ERR_WOULD_BLOCK) {
                    return result;
                }
            }
            if (!server_complete && now_ms == server_deadline) {
                result = h2_pal_dtls_session_handshake(
                    h2_wolfssl_dtls_api(), server, NULL, 0u, now_ms,
                    TEST_HANDSHAKE_DEADLINE_MS, &server_complete);
                if (result != H2_PAL_OK &&
                    result != H2_PAL_ERR_WOULD_BLOCK) {
                    return result;
                }
            }
        }
        ++now_ms;
    }
    return client_complete && server_complete
               ? H2_PAL_OK
               : H2_PAL_ERR_TIMEOUT;
}

static void destroy_pair(
    h2_pal_dtls_session_t **client,
    h2_pal_dtls_session_t **server) {
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), client);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), server);
}

static void assert_zero(const uint8_t *data, size_t len) {
    size_t index;
    for (index = 0u; index < len; ++index) {
        assert(data[index] == 0u);
    }
}

static void test_session_create_failures(
    test_alloc_state_t *alloc_state,
    test_entropy_state_t *entropy_state) {
    test_queue_t outgoing = {0};
    test_endpoint_t endpoint = {.outgoing = &outgoing};
    h2_pal_dtls_session_config_t config = {
        .role = H2_PAL_DTLS_ROLE_CLIENT,
        .max_datagram_size = TEST_PACKET_SIZE,
        .max_plaintext_size = 1024u,
        .max_pending_output_bytes = TEST_PENDING_CAPACITY,
        .send = queue_send,
        .plaintext = collect_plaintext,
        .io_user = &endpoint,
    };
    h2_pal_dtls_session_t *session = NULL;
    size_t baseline_live = alloc_state->live;

    alloc_state->fail_at = alloc_state->calls + 1u;
    assert(h2_pal_dtls_session_create(
               h2_wolfssl_dtls_api(), &config, &session) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(session == NULL);
    assert(alloc_state->live == baseline_live);
    alloc_state->fail_at = 0u;

    entropy_state->fail = 1;
    assert(h2_pal_dtls_session_create(
               h2_wolfssl_dtls_api(), &config, &session) == H2_PAL_ERR_IO);
    assert(session == NULL);
    assert(alloc_state->live == baseline_live);
    entropy_state->fail = 0;

    assert(h2_pal_dtls_session_create(
               h2_wolfssl_dtls_api(), &config, &session) == H2_PAL_OK);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &session);
    assert(alloc_state->live == baseline_live);
}

static void test_fingerprint_state(void) {
    test_queue_t outgoing = {0};
    test_endpoint_t endpoint = {.outgoing = &outgoing};
    h2_pal_dtls_session_t *session = create_session(
        H2_PAL_DTLS_ROLE_CLIENT, &endpoint, TEST_PENDING_CAPACITY);
    uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE] = {0};
    uint8_t different[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE] = {0};
    uint8_t exporter[H2_PAL_DTLS_SRTP_KEYING_MATERIAL_SIZE];
    h2_pal_dtls_srtp_profile_t profile =
        H2_PAL_DTLS_SRTP_PROFILE_AES128_CM_SHA1_80;
    int complete = 1;

    assert(h2_pal_dtls_session_handshake(
               h2_wolfssl_dtls_api(), session, NULL, 0u, 1u,
               TEST_HANDSHAKE_DEADLINE_MS, &complete) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(complete == 0);
    memset(exporter, 0xa5, sizeof(exporter));
    assert(h2_pal_dtls_session_export_srtp_keying_material(
               h2_wolfssl_dtls_api(), session,
               exporter, sizeof(exporter)) == H2_PAL_ERR_INVALID_STATE);
    assert_zero(exporter, sizeof(exporter));
    assert(h2_pal_dtls_session_get_srtp_profile(
               h2_wolfssl_dtls_api(), session, &profile) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(profile == (h2_pal_dtls_srtp_profile_t)0);

    different[0] = 1u;
    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), session, fingerprint) == H2_PAL_OK);
    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), session, fingerprint) == H2_PAL_OK);
    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), session, different) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_dtls_session_handshake(
               h2_wolfssl_dtls_api(), session, NULL, 0u, 1u,
               TEST_HANDSHAKE_DEADLINE_MS, &complete) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), session, fingerprint) ==
           H2_PAL_ERR_INVALID_STATE);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &session);
}

static void test_hard_timeout(void) {
    test_queue_t outgoing = {0};
    test_endpoint_t endpoint = {.outgoing = &outgoing};
    h2_pal_dtls_session_t *session = create_session(
        H2_PAL_DTLS_ROLE_CLIENT, &endpoint, TEST_PENDING_CAPACITY);
    uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE] = {0};
    int complete = 1;

    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), session, fingerprint) == H2_PAL_OK);
    assert(h2_pal_dtls_session_handshake(
               h2_wolfssl_dtls_api(), session, NULL, 0u,
               10u, 10u, &complete) == H2_PAL_ERR_TIMEOUT);
    assert(complete == 0);
    assert(h2_pal_dtls_session_flush(
               h2_wolfssl_dtls_api(), session) == H2_PAL_ERR_TIMEOUT);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &session);
}

static void test_malformed_datagram(void) {
    static const uint8_t malformed[] = {
        22u, 0xfeu, 0xfdu, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 1u, 0xffu,
    };
    test_queue_t outgoing = {0};
    test_endpoint_t endpoint = {.outgoing = &outgoing};
    h2_pal_dtls_session_t *session = create_session(
        H2_PAL_DTLS_ROLE_SERVER, &endpoint, TEST_PENDING_CAPACITY);
    uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE] = {0};
    h2_pal_result_t result;
    int complete = 0;

    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), session, fingerprint) == H2_PAL_OK);
    result = h2_pal_dtls_session_handshake(
        h2_wolfssl_dtls_api(), session, malformed, sizeof(malformed),
        1u, TEST_HANDSHAKE_DEADLINE_MS, &complete);
    assert(result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_pal_dtls_session_flush(
               h2_wolfssl_dtls_api(), session) == result);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &session);
}

static void test_handshake_output_failures(void) {
    test_queue_t blocked_outgoing = {0};
    test_endpoint_t blocked_endpoint = {
        .outgoing = &blocked_outgoing,
        .send_result = H2_PAL_ERR_WOULD_BLOCK,
    };
    test_queue_t failed_outgoing = {0};
    test_endpoint_t failed_endpoint = {
        .outgoing = &failed_outgoing,
        .send_result = H2_PAL_ERR_IO,
    };
    uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE] = {0};
    h2_pal_dtls_session_t *blocked = create_session(
        H2_PAL_DTLS_ROLE_CLIENT, &blocked_endpoint, TEST_PENDING_CAPACITY);
    h2_pal_dtls_session_t *failed = create_session(
        H2_PAL_DTLS_ROLE_CLIENT, &failed_endpoint, TEST_PENDING_CAPACITY);
    h2_pal_dtls_session_t *bounded = create_session(
        H2_PAL_DTLS_ROLE_CLIENT, &blocked_endpoint, 1u);
    int complete = 0;

    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), blocked, fingerprint) == H2_PAL_OK);
    assert(h2_pal_dtls_session_handshake(
               h2_wolfssl_dtls_api(), blocked, NULL, 0u, 1u,
               TEST_HANDSHAKE_DEADLINE_MS, &complete) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_pal_dtls_session_flush(
               h2_wolfssl_dtls_api(), blocked) == H2_PAL_ERR_WOULD_BLOCK);
    blocked_endpoint.send_result = H2_PAL_OK;
    assert(h2_pal_dtls_session_flush(
               h2_wolfssl_dtls_api(), blocked) == H2_PAL_OK);
    assert(blocked_outgoing.count > 0u);

    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), failed, fingerprint) == H2_PAL_OK);
    assert(h2_pal_dtls_session_handshake(
               h2_wolfssl_dtls_api(), failed, NULL, 0u, 1u,
               TEST_HANDSHAKE_DEADLINE_MS, &complete) == H2_PAL_ERR_IO);
    assert(h2_pal_dtls_session_flush(
               h2_wolfssl_dtls_api(), failed) == H2_PAL_ERR_IO);

    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), bounded, fingerprint) == H2_PAL_OK);
    assert(h2_pal_dtls_session_handshake(
               h2_wolfssl_dtls_api(), bounded, NULL, 0u, 1u,
               TEST_HANDSHAKE_DEADLINE_MS, &complete) ==
           H2_PAL_ERR_NO_SPACE);
    assert(h2_pal_dtls_session_flush(
               h2_wolfssl_dtls_api(), bounded) == H2_PAL_ERR_NO_SPACE);

    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &blocked);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &failed);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &bounded);
}

static void test_fingerprint_failure(void) {
    test_queue_t client_to_server = {0};
    test_queue_t server_to_client = {0};
    test_endpoint_t client_endpoint = {.outgoing = &client_to_server};
    test_endpoint_t server_endpoint = {.outgoing = &server_to_client};
    h2_pal_dtls_session_t *client = create_session(
        H2_PAL_DTLS_ROLE_CLIENT, &client_endpoint, TEST_PENDING_CAPACITY);
    h2_pal_dtls_session_t *server = create_session(
        H2_PAL_DTLS_ROLE_SERVER, &server_endpoint, TEST_PENDING_CAPACITY);

    configure_pair(client, server, 1);
    assert(drive_pair(
               client, server, &client_to_server, &server_to_client) ==
           H2_PAL_ERR_TLS_VERIFY);
    assert(h2_pal_dtls_session_flush(
               h2_wolfssl_dtls_api(), client) == H2_PAL_ERR_TLS_VERIFY);
    destroy_pair(&client, &server);
}

static void test_plaintext_callback_failure(void) {
    static const uint8_t message[] = "callback failure";
    uint8_t oversized[1025] = {0};
    test_queue_t client_to_server = {0};
    test_queue_t server_to_client = {0};
    test_endpoint_t client_endpoint = {.outgoing = &client_to_server};
    test_endpoint_t server_endpoint = {.outgoing = &server_to_client};
    h2_pal_dtls_session_t *client = create_session(
        H2_PAL_DTLS_ROLE_CLIENT, &client_endpoint, TEST_PENDING_CAPACITY);
    h2_pal_dtls_session_t *server = create_session(
        H2_PAL_DTLS_ROLE_SERVER, &server_endpoint, TEST_PENDING_CAPACITY);
    test_packet_t packet;

    configure_pair(client, server, 0);
    assert(drive_pair(
               client, server, &client_to_server, &server_to_client) ==
           H2_PAL_OK);
    assert(h2_pal_dtls_session_write(
               h2_wolfssl_dtls_api(), client,
               oversized, sizeof(oversized)) == H2_PAL_ERR_TRUNCATED);
    server_endpoint.plaintext_result = H2_PAL_ERR_IO;
    assert(h2_pal_dtls_session_write(
               h2_wolfssl_dtls_api(), client,
               message, sizeof(message) - 1u) == H2_PAL_OK);
    assert(queue_pop(&client_to_server, &packet));
    assert(h2_pal_dtls_session_consume_datagram(
               h2_wolfssl_dtls_api(), server,
               packet.bytes, packet.len) == H2_PAL_ERR_IO);
    assert(h2_pal_dtls_session_flush(
               h2_wolfssl_dtls_api(), server) == H2_PAL_ERR_IO);
    destroy_pair(&client, &server);
}

static void test_close_alert(void) {
    test_queue_t client_to_server = {0};
    test_queue_t server_to_client = {0};
    test_endpoint_t client_endpoint = {.outgoing = &client_to_server};
    test_endpoint_t server_endpoint = {.outgoing = &server_to_client};
    h2_pal_dtls_session_t *client = create_session(
        H2_PAL_DTLS_ROLE_CLIENT, &client_endpoint, TEST_PENDING_CAPACITY);
    h2_pal_dtls_session_t *server = create_session(
        H2_PAL_DTLS_ROLE_SERVER, &server_endpoint, TEST_PENDING_CAPACITY);
    test_packet_t packet;
    h2_pal_result_t result;
    int saw_closed = 0;

    configure_pair(client, server, 0);
    assert(drive_pair(
               client, server, &client_to_server, &server_to_client) ==
           H2_PAL_OK);
    assert(h2_pal_dtls_session_close(
               h2_wolfssl_dtls_api(), client) == H2_PAL_OK);
    assert(h2_pal_dtls_session_close(
               h2_wolfssl_dtls_api(), client) == H2_PAL_OK);
    assert(h2_pal_dtls_session_write(
               h2_wolfssl_dtls_api(), client, NULL, 0u) ==
           H2_PAL_ERR_INVALID_STATE);
    while (queue_pop(&client_to_server, &packet)) {
        result = h2_pal_dtls_session_consume_datagram(
            h2_wolfssl_dtls_api(), server, packet.bytes, packet.len);
        assert(result == H2_PAL_OK || result == H2_PAL_ERR_CLOSED);
        saw_closed |= result == H2_PAL_ERR_CLOSED;
    }
    assert(saw_closed);
    destroy_pair(&client, &server);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &client);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &server);
}

int main(void) {
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_alloc,
        .realloc = test_realloc,
        .free = test_free,
    };
    test_alloc_state_t alloc_state = {0};
    test_entropy_state_t entropy_state = {.value = 17u};
    h2_wolfssl_config_t provider_config = {
        .mem = {
            .user = &alloc_state,
            .vtable = &mem_vtable,
        },
        .entropy_user = &entropy_state,
        .entropy = test_entropy,
    };

    assert(h2_wolfssl_init(&provider_config) == H2_PAL_OK);
    test_session_create_failures(&alloc_state, &entropy_state);
    test_fingerprint_state();
    test_hard_timeout();
    test_malformed_datagram();
    test_handshake_output_failures();
    test_fingerprint_failure();
    test_plaintext_callback_failure();
    test_close_alert();
    assert(h2_wolfssl_deinit() == H2_PAL_OK);
    assert(alloc_state.live == 0u);
    return 0;
}
