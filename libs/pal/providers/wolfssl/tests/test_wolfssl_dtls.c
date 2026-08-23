#include "h2_wolfssl.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PACKET_CAPACITY 64u
#define TEST_PACKET_SIZE 1500u

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
    int block_send;
    uint8_t plaintext[256];
    size_t plaintext_len;
} test_endpoint_t;

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

static int test_entropy(void *user, uint8_t *out, size_t len) {
    uint32_t *state = user;
    size_t index;
    for (index = 0u; index < len; ++index) {
        *state = *state * 1664525u + 1013904223u;
        out[index] = (uint8_t)(*state >> 24u);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t queue_send(
    void *user, const uint8_t *data, size_t len) {
    test_endpoint_t *endpoint = user;
    size_t tail;
    if (endpoint->block_send) {
        return H2_PAL_ERR_WOULD_BLOCK;
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
    assert(len <= sizeof(endpoint->plaintext) - endpoint->plaintext_len);
    memcpy(endpoint->plaintext + endpoint->plaintext_len, data, len);
    endpoint->plaintext_len += len;
    return H2_PAL_OK;
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
    h2_pal_dtls_role_t role, test_endpoint_t *endpoint) {
    h2_pal_dtls_session_config_t config = {
        .role = role,
        .max_datagram_size = TEST_PACKET_SIZE,
        .max_plaintext_size = 1024u,
        .max_pending_output_bytes = 16384u,
        .send = queue_send,
        .plaintext = collect_plaintext,
        .io_user = endpoint,
    };
    h2_pal_dtls_session_t *session = NULL;
    assert(h2_pal_dtls_session_create(
               h2_wolfssl_dtls_api(), &config, &session) == H2_PAL_OK);
    return session;
}

static void drive_handshake(
    h2_pal_dtls_session_t *client,
    h2_pal_dtls_session_t *server,
    test_queue_t *client_to_server,
    test_queue_t *server_to_client) {
    test_packet_t packet;
    uint64_t now_ms = 1u;
    uint64_t completed_deadline = 1u;
    int client_complete = 0;
    int server_complete = 0;
    int iterations;
    h2_pal_result_t result;

    result = h2_pal_dtls_session_handshake(
        h2_wolfssl_dtls_api(), client, NULL, 0u,
        now_ms, 60000u, &client_complete);
    assert(result == H2_PAL_ERR_WOULD_BLOCK);
    result = h2_pal_dtls_session_handshake(
        h2_wolfssl_dtls_api(), client, NULL, 0u,
        now_ms, 60001u, &client_complete);
    assert(result == H2_PAL_ERR_INVALID_ARG);
    for (iterations = 0; iterations < 1000 &&
                         (!client_complete || !server_complete);
         ++iterations) {
        int progressed = 0;
        while (queue_pop(client_to_server, &packet)) {
            result = h2_pal_dtls_session_handshake(
                h2_wolfssl_dtls_api(), server, packet.bytes, packet.len,
                now_ms, 60000u, &server_complete);
            assert(result == H2_PAL_OK || result == H2_PAL_ERR_WOULD_BLOCK);
            progressed = 1;
        }
        while (queue_pop(server_to_client, &packet)) {
            result = h2_pal_dtls_session_handshake(
                h2_wolfssl_dtls_api(), client, packet.bytes, packet.len,
                now_ms, 60000u, &client_complete);
            assert(result == H2_PAL_OK || result == H2_PAL_ERR_WOULD_BLOCK);
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
                    h2_wolfssl_dtls_api(), client, NULL, 0u,
                    now_ms, 60000u, &client_complete);
                assert(result == H2_PAL_OK ||
                       result == H2_PAL_ERR_WOULD_BLOCK);
            }
            if (!server_complete && now_ms == server_deadline) {
                result = h2_pal_dtls_session_handshake(
                    h2_wolfssl_dtls_api(), server, NULL, 0u,
                    now_ms, 60000u, &server_complete);
                assert(result == H2_PAL_OK ||
                       result == H2_PAL_ERR_WOULD_BLOCK);
            }
        }
        ++now_ms;
    }
    assert(client_complete);
    assert(server_complete);
    assert(h2_pal_dtls_session_handshake(
               h2_wolfssl_dtls_api(), client, NULL, 0u,
               now_ms, 60001u, &client_complete) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_dtls_session_next_deadline_ms(
               h2_wolfssl_dtls_api(), client, &completed_deadline) ==
           H2_PAL_ERR_NOT_FOUND);
    assert(completed_deadline == 0u);
}

int main(void) {
    static const h2_pal_mem_vtable_t mem_vtable = {
        .alloc = test_alloc,
        .realloc = test_realloc,
        .free = test_free,
    };
    uint32_t entropy_state = 7u;
    h2_wolfssl_config_t provider_config = {
        .mem = {
            .user = NULL,
            .vtable = &mem_vtable,
        },
        .entropy_user = &entropy_state,
        .entropy = test_entropy,
    };
    test_queue_t client_to_server = {0};
    test_queue_t server_to_client = {0};
    test_endpoint_t client_endpoint = {
        .outgoing = &client_to_server,
    };
    test_endpoint_t server_endpoint = {
        .outgoing = &server_to_client,
    };
    h2_pal_dtls_session_t *client;
    h2_pal_dtls_session_t *server;
    uint8_t client_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    uint8_t server_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    uint8_t client_keying_material[H2_PAL_DTLS_SRTP_KEYING_MATERIAL_SIZE];
    uint8_t server_keying_material[H2_PAL_DTLS_SRTP_KEYING_MATERIAL_SIZE];
    h2_pal_dtls_srtp_profile_t profile;
    test_packet_t packet;
    static const uint8_t first_message[] = "hello";
    static const uint8_t second_message[] = "blocked";

    assert(h2_wolfssl_init(&provider_config) == H2_PAL_OK);
    client = create_session(H2_PAL_DTLS_ROLE_CLIENT, &client_endpoint);
    server = create_session(H2_PAL_DTLS_ROLE_SERVER, &server_endpoint);
    assert(h2_pal_dtls_session_get_local_fingerprint(
               h2_wolfssl_dtls_api(), client, client_fingerprint) ==
           H2_PAL_OK);
    assert(h2_pal_dtls_session_get_local_fingerprint(
               h2_wolfssl_dtls_api(), server, server_fingerprint) ==
           H2_PAL_OK);
    assert(memcmp(
               client_fingerprint, server_fingerprint,
               sizeof(client_fingerprint)) != 0);
    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), client, server_fingerprint) ==
           H2_PAL_OK);
    assert(h2_pal_dtls_session_set_remote_fingerprint(
               h2_wolfssl_dtls_api(), server, client_fingerprint) ==
           H2_PAL_OK);

    drive_handshake(
        client, server, &client_to_server, &server_to_client);
    assert(h2_pal_dtls_session_get_srtp_profile(
               h2_wolfssl_dtls_api(), client, &profile) == H2_PAL_OK);
    assert(profile == H2_PAL_DTLS_SRTP_PROFILE_AES128_CM_SHA1_80);
    assert(h2_pal_dtls_session_export_srtp_keying_material(
               h2_wolfssl_dtls_api(), client,
               client_keying_material,
               sizeof(client_keying_material) - 1u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_dtls_session_export_srtp_keying_material(
               h2_wolfssl_dtls_api(), client,
               client_keying_material, sizeof(client_keying_material)) ==
           H2_PAL_OK);
    assert(h2_pal_dtls_session_export_srtp_keying_material(
               h2_wolfssl_dtls_api(), server,
               server_keying_material, sizeof(server_keying_material)) ==
           H2_PAL_OK);
    assert(memcmp(
               client_keying_material, server_keying_material,
               sizeof(client_keying_material)) == 0);

    assert(h2_pal_dtls_session_write(
               h2_wolfssl_dtls_api(), client,
               first_message, sizeof(first_message) - 1u) == H2_PAL_OK);
    while (queue_pop(&client_to_server, &packet)) {
        assert(h2_pal_dtls_session_consume_datagram(
                   h2_wolfssl_dtls_api(), server,
                   packet.bytes, packet.len) == H2_PAL_OK);
    }
    assert(server_endpoint.plaintext_len == sizeof(first_message) - 1u);
    assert(memcmp(
               server_endpoint.plaintext, first_message,
               sizeof(first_message) - 1u) == 0);

    client_endpoint.block_send = 1;
    assert(h2_pal_dtls_session_write(
               h2_wolfssl_dtls_api(), client,
               second_message, sizeof(second_message) - 1u) == H2_PAL_OK);
    assert(h2_pal_dtls_session_write(
               h2_wolfssl_dtls_api(), client,
               second_message, sizeof(second_message) - 1u) ==
           H2_PAL_ERR_WOULD_BLOCK);
    client_endpoint.block_send = 0;
    assert(h2_pal_dtls_session_flush(
               h2_wolfssl_dtls_api(), client) == H2_PAL_OK);
    while (queue_pop(&client_to_server, &packet)) {
        assert(h2_pal_dtls_session_consume_datagram(
                   h2_wolfssl_dtls_api(), server,
                   packet.bytes, packet.len) == H2_PAL_OK);
    }
    assert(server_endpoint.plaintext_len ==
           sizeof(first_message) + sizeof(second_message) - 2u);
    assert(memcmp(
               server_endpoint.plaintext + sizeof(first_message) - 1u,
               second_message, sizeof(second_message) - 1u) == 0);

    assert(h2_pal_dtls_session_close(
               h2_wolfssl_dtls_api(), client) == H2_PAL_OK);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &client);
    h2_pal_dtls_session_destroy(h2_wolfssl_dtls_api(), &server);
    assert(h2_wolfssl_deinit() == H2_PAL_OK);
    return 0;
}
