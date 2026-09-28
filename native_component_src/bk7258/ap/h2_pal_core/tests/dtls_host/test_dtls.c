#include "h2_bk_platform_core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>

/* This test replaces only host memory, OS entropy, logging and the datagram
 * transport. Certificates, cookies, handshake, SRTP exporter and records run
 * through the actual BK provider and SDK mbedTLS implementation. */
static unsigned live_allocations;
static void *host_alloc(void *user, size_t size) {
    (void)user;
    void *memory = malloc(size);
    if (memory != NULL) ++live_allocations;
    return memory;
}
static void host_free(void *user, void *memory) {
    (void)user;
    if (memory != NULL) {
        assert(live_allocations > 0u);
        --live_allocations;
        free(memory);
    }
}
h2_pal_mem_api_t *h2_bk_platform_default_allocator(void) {
    static const h2_pal_mem_vtable_t vtable = {
        .alloc = host_alloc, .free = host_free};
    static h2_pal_mem_api_t api = {NULL, &vtable};
    return &api;
}
static int host_random(void *user, uint8_t *bytes, size_t size) {
    (void)user;
    FILE *source = fopen("/dev/urandom", "rb");
    if (source == NULL) return H2_PAL_ERR_IO;
    size_t read = fread(bytes, 1u, size, source);
    fclose(source);
    return read == size ? H2_PAL_OK : H2_PAL_ERR_IO;
}
const h2_pal_crypto_api_t *h2_bk_platform_crypto_api(void) {
    static const h2_pal_crypto_vtable_t vtable = {.random = host_random};
    static const h2_pal_crypto_api_t api = {NULL, &vtable};
    return &api;
}
static int host_log(void *user, h2_pal_log_level_t level,
                    const char *scope, const char *message) {
    (void)user;
    (void)level;
    fprintf(stderr, "%s %s\n", scope, message);
    return H2_PAL_OK;
}
const h2_pal_log_api_t *h2_bk_platform_log_api(void) {
    static const h2_pal_log_vtable_t vtable = {.write = host_log};
    static const h2_pal_log_api_t api = {NULL, &vtable};
    return &api;
}

typedef struct packet {
    size_t length;
    uint8_t bytes[2048];
} packet_t;
typedef struct endpoint {
    packet_t queue[64];
    unsigned read, written, hello_verify;
    int drop_cookie, blocked;
    uint8_t received[64];
    size_t received_length;
} endpoint_t;
static h2_pal_result_t send_packet(void *user, const uint8_t *bytes, size_t length) {
    endpoint_t *endpoint = user;
    if (endpoint->blocked) {
        endpoint->blocked = 0;
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    /* A DTLS 1.2 handshake record whose first message is HelloVerifyRequest. */
    if (length > 13u && bytes[0] == 22u && bytes[13] == 3u) {
        ++endpoint->hello_verify;
        if (endpoint->drop_cookie) {
            endpoint->drop_cookie = 0;
            return H2_PAL_OK;
        }
    }
    assert(length <= sizeof(endpoint->queue[0].bytes));
    assert(endpoint->written - endpoint->read < 64u);
    packet_t *packet = &endpoint->queue[endpoint->written++ % 64u];
    packet->length = length;
    memcpy(packet->bytes, bytes, length);
    return H2_PAL_OK;
}
static h2_pal_result_t plaintext(void *user, const uint8_t *bytes, size_t length) {
    endpoint_t *endpoint = user;
    assert(length <= sizeof(endpoint->received));
    memcpy(endpoint->received, bytes, length);
    endpoint->received_length = length;
    return H2_PAL_OK;
}
static packet_t *receive(endpoint_t *endpoint) {
    return endpoint->read < endpoint->written
               ? &endpoint->queue[endpoint->read++ % 64u] : NULL;
}
static int handshake(const h2_pal_dtls_api_t *api, h2_pal_dtls_session_t *session,
                     endpoint_t *remote, uint64_t now, int *complete) {
    if (*complete) return H2_PAL_OK;
    packet_t *packet = receive(remote);
    return h2_pal_dtls_session_handshake(api, session,
        packet != NULL ? packet->bytes : NULL,
        packet != NULL ? packet->length : 0u, now, 20000u, complete);
}
static void drain(const h2_pal_dtls_api_t *api, h2_pal_dtls_session_t *session,
                  endpoint_t *remote) {
    packet_t *packet;
    while ((packet = receive(remote)) != NULL) {
        assert(h2_pal_dtls_session_consume_datagram(
            api, session, packet->bytes, packet->length) == H2_PAL_OK);
    }
}

static void run(int wrong_fingerprint, int drop_cookie) {
    endpoint_t client_output = {0}, server_output = {.drop_cookie = drop_cookie};
    const h2_pal_dtls_api_t *api = h2_bk_platform_dtls_api();
    h2_pal_dtls_session_t *client = NULL, *server = NULL;
    h2_pal_dtls_session_config_t config = {
        .role = H2_PAL_DTLS_ROLE_CLIENT, .max_datagram_size = 1200u,
        .max_plaintext_size = 1200u, .max_pending_output_bytes = 4800u,
        .send = send_packet, .plaintext = plaintext, .io_user = &client_output};
    assert(h2_pal_dtls_session_create(api, &config, &client) == H2_PAL_OK);
    config.role = H2_PAL_DTLS_ROLE_SERVER;
    config.io_user = &server_output;
    assert(h2_pal_dtls_session_create(api, &config, &server) == H2_PAL_OK);
    uint8_t fingerprint[32];
    assert(h2_pal_dtls_session_get_local_fingerprint(api, client, fingerprint) == 0);
    if (wrong_fingerprint == 1) fingerprint[0] ^= 1u;
    assert(h2_pal_dtls_session_set_remote_fingerprint(api, server, fingerprint) == 0);
    assert(h2_pal_dtls_session_get_local_fingerprint(api, server, fingerprint) == 0);
    if (wrong_fingerprint == 2) fingerprint[0] ^= 1u;
    assert(h2_pal_dtls_session_set_remote_fingerprint(api, client, fingerprint) == 0);

    int client_complete = 0, server_complete = 0, result = H2_PAL_OK;
    for (uint64_t now = 0; now < 20000u &&
         (!client_complete || !server_complete); now += 10u) {
        result = handshake(api, client, &server_output, now, &client_complete);
        if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) break;
        result = handshake(api, server, &client_output, now, &server_complete);
        if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) break;
    }
    assert(server_output.hello_verify >= (drop_cookie ? 2u : 1u));
    if (wrong_fingerprint) {
        assert(result == H2_PAL_ERR_TLS_VERIFY);
        assert(wrong_fingerprint == 1 ? !server_complete : !client_complete);
        uint8_t keys[H2_PAL_DTLS_SRTP_KEYING_MATERIAL_SIZE];
        assert(h2_pal_dtls_session_export_srtp_keying_material(api,
            wrong_fingerprint == 1 ? server : client, keys, sizeof(keys)) ==
            H2_PAL_ERR_INVALID_STATE);
        assert(client_output.received_length == 0 && server_output.received_length == 0);
    } else {
        assert(client_complete && server_complete);
        uint8_t client_keys[60], server_keys[60];
        assert(h2_pal_dtls_session_export_srtp_keying_material(
            api, client, client_keys, sizeof(client_keys)) == H2_PAL_OK);
        assert(h2_pal_dtls_session_export_srtp_keying_material(
            api, server, server_keys, sizeof(server_keys)) == H2_PAL_OK);
        assert(memcmp(client_keys, server_keys, sizeof(client_keys)) == 0);
        const uint8_t payload[] = {0u, 1u, 128u, 255u, 0u};
        client_output.blocked = 1;
        assert(h2_pal_dtls_session_write(api, client, payload, sizeof(payload)) ==
               H2_PAL_ERR_WOULD_BLOCK);
        assert(h2_pal_dtls_session_write(api, client, payload, sizeof(payload)) == H2_PAL_OK);
        drain(api, server, &client_output);
        assert(server_output.received_length == sizeof(payload));
        assert(memcmp(server_output.received, payload, sizeof(payload)) == 0);
        assert(h2_pal_dtls_session_write(api, server, payload, sizeof(payload)) == H2_PAL_OK);
        drain(api, client, &server_output);
        assert(client_output.received_length == sizeof(payload));
        assert(memcmp(client_output.received, payload, sizeof(payload)) == 0);
    }
    assert(h2_pal_dtls_session_close(api, client) == H2_PAL_OK);
    assert(h2_pal_dtls_session_close(api, client) == H2_PAL_OK);
    h2_pal_dtls_session_destroy(api, &client);
    h2_pal_dtls_session_destroy(api, &server);
    assert(client == NULL && server == NULL && live_allocations == 0u);
    printf("BK_DTLS_HOST_PASS wrong_fingerprint=%d drop_cookie=%d\n",
           wrong_fingerprint, drop_cookie);
}

/* A real mbedTLS client deliberately omits its own certificate. It must not
 * turn the provider's deferred CA-chain check into anonymous authentication. */
typedef struct anonymous_client {
    endpoint_t output;
    packet_t *input;
    uint64_t now, timer_start;
    uint32_t intermediate, final;
} anonymous_client_t;
static int anonymous_send(void *user, const unsigned char *bytes, size_t size) {
    anonymous_client_t *client = user;
    return send_packet(&client->output, bytes, size) == H2_PAL_OK ? (int)size : -1;
}
static int anonymous_recv(void *user, unsigned char *bytes, size_t capacity) {
    anonymous_client_t *client = user;
    if (client->input == NULL) return MBEDTLS_ERR_SSL_WANT_READ;
    assert(client->input->length <= capacity);
    size_t length = client->input->length;
    memcpy(bytes, client->input->bytes, length);
    client->input = NULL;
    return (int)length;
}
static void timer_set(void *user, uint32_t intermediate, uint32_t final) {
    anonymous_client_t *client = user;
    client->timer_start = client->now;
    client->intermediate = intermediate;
    client->final = final;
}
static int timer_get(void *user) {
    anonymous_client_t *client = user;
    uint64_t elapsed = client->now - client->timer_start;
    if (client->final == 0u) return -1;
    return elapsed >= client->final ? 2 : elapsed >= client->intermediate ? 1 : 0;
}
static void missing_certificate(void) {
    const h2_pal_dtls_api_t *api = h2_bk_platform_dtls_api();
    anonymous_client_t client = {0};
    endpoint_t server_output = {0};
    h2_pal_dtls_session_t *server = NULL;
    h2_pal_dtls_session_config_t config = {
        .role = H2_PAL_DTLS_ROLE_SERVER, .max_datagram_size = 1200u,
        .max_plaintext_size = 1200u, .max_pending_output_bytes = 4800u,
        .send = send_packet, .plaintext = plaintext, .io_user = &server_output};
    assert(h2_pal_dtls_session_create(api, &config, &server) == H2_PAL_OK);
    uint8_t expected[32] = {1u};
    assert(h2_pal_dtls_session_set_remote_fingerprint(api, server, expected) == H2_PAL_OK);
    assert(h2_pal_dtls_session_get_local_fingerprint(api, server, expected) == H2_PAL_OK);

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_config;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&ssl_config);
    assert(mbedtls_ssl_config_defaults(&ssl_config, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0);
    mbedtls_ssl_conf_rng(&ssl_config, host_random, NULL);
    mbedtls_ssl_conf_authmode(&ssl_config, MBEDTLS_SSL_VERIFY_OPTIONAL);
    static const mbedtls_ssl_srtp_profile profiles[] = {
        MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80, MBEDTLS_TLS_SRTP_UNSET};
    assert(mbedtls_ssl_conf_dtls_srtp_protection_profiles(&ssl_config, profiles) == 0);
    assert(mbedtls_ssl_setup(&ssl, &ssl_config) == 0);
    mbedtls_ssl_set_bio(&ssl, &client, anonymous_send, anonymous_recv, NULL);
    mbedtls_ssl_set_timer_cb(&ssl, &client, timer_set, timer_get);

    int complete = 0, result = H2_PAL_OK;
    for (client.now = 0u; client.now < 20000u; client.now += 10u) {
        client.input = receive(&server_output);
        int rc = mbedtls_ssl_handshake(&ssl);
        client.input = NULL;
        assert(rc == 0 || rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
        result = handshake(api, server, &client.output, client.now, &complete);
        if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) break;
    }
    assert(server_output.hello_verify > 0u);
    assert(result == H2_PAL_ERR_TLS_VERIFY && !complete);
    /* The server rejects its missing peer certificate after emitting the
     * final flight. Finish only the adversarial client's handshake so its
     * session exposes the already received server certificate for our check. */
    int client_complete = 0;
    for (; client.now < 20000u; client.now += 10u) {
        client.input = receive(&server_output);
        int rc = mbedtls_ssl_handshake(&ssl);
        client.input = NULL;
        if (rc == 0) {
            client_complete = 1;
            break;
        }
        assert(rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    }
    assert(client_complete);
    const mbedtls_x509_crt *certificate = mbedtls_ssl_get_peer_cert(&ssl);
    assert(certificate != NULL);
    uint8_t fingerprint[32], keys[60];
    assert(mbedtls_sha256(certificate->raw.p, certificate->raw.len, fingerprint, 0) == 0);
    assert(memcmp(fingerprint, expected, sizeof(expected)) == 0);
    assert(h2_pal_dtls_session_export_srtp_keying_material(
        api, server, keys, sizeof(keys)) == H2_PAL_ERR_INVALID_STATE);
    assert(server_output.received_length == 0u);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&ssl_config);
    h2_pal_dtls_session_destroy(api, &server);
    assert(server == NULL && live_allocations == 0u);
    puts("BK_DTLS_HOST_PASS missing_peer_certificate");
}
int main(void) {
    run(0, 0);
    run(0, 1);
    run(1, 0);
    run(2, 0);
    missing_certificate();
    return 0;
}
