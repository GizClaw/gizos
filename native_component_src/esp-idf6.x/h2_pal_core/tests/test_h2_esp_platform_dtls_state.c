#include "h2_esp_platform_dtls_state.h"

#include <assert.h>
#include <string.h>

static void test_peer_fingerprint(void) {
    h2_esp_dtls_captured_state_t state = {0};
    uint8_t fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    uint8_t mismatch[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    memset(fingerprint, 0x5au, sizeof(fingerprint));
    memcpy(mismatch, fingerprint, sizeof(mismatch));
    mismatch[0] ^= 0xffu;

    assert(h2_esp_dtls_captured_state_verify_peer(&state, fingerprint) ==
           H2_PAL_ERR_TLS_VERIFY);
    assert(h2_esp_dtls_captured_state_set_peer(
               &state, fingerprint, sizeof(fingerprint) - 1u) ==
           H2_PAL_ERR_TRUNCATED);
    assert(h2_esp_dtls_captured_state_set_peer(
               &state, fingerprint, sizeof(fingerprint)) == H2_PAL_OK);
    memset(fingerprint, 0u, sizeof(fingerprint));
    assert(h2_esp_dtls_captured_state_verify_peer(&state, mismatch) ==
           H2_PAL_ERR_TLS_VERIFY);
    mismatch[0] ^= 0xffu;
    assert(h2_esp_dtls_captured_state_verify_peer(&state, mismatch) ==
           H2_PAL_OK);
    h2_esp_dtls_captured_state_reset_peer(&state);
    assert(h2_esp_dtls_captured_state_verify_peer(&state, mismatch) ==
           H2_PAL_ERR_TLS_VERIFY);
}

static void test_exporter_capture_lifetime(void) {
    h2_esp_dtls_captured_state_t state = {0};
    uint8_t secret[H2_ESP_DTLS_MASTER_SECRET_CAPACITY];
    uint8_t client_random[32];
    uint8_t server_random[32];
    memset(secret, 0x11u, sizeof(secret));
    memset(client_random, 0x22u, sizeof(client_random));
    memset(server_random, 0x33u, sizeof(server_random));

    assert(h2_esp_dtls_captured_state_set_exporter(
               &state, secret, sizeof(secret) + 1u, client_random,
               server_random, 7) == H2_PAL_ERR_NO_SPACE);
    assert(!state.exporter_ready);
    assert(h2_esp_dtls_captured_state_set_exporter(
               &state, secret, sizeof(secret), client_random,
               server_random, 7) == H2_PAL_OK);
    assert(state.exporter_ready);
    assert(state.master_secret_len == sizeof(secret));
    assert(state.prf == 7);

    memset(secret, 0u, sizeof(secret));
    memset(client_random, 0u, sizeof(client_random));
    memset(server_random, 0u, sizeof(server_random));
    for (size_t i = 0u; i < sizeof(state.master_secret); ++i) {
        assert(state.master_secret[i] == 0x11u);
    }
    for (size_t i = 0u; i < 32u; ++i) {
        assert(state.randoms[i] == 0x22u);
        assert(state.randoms[i + 32u] == 0x33u);
    }
}

int main(void) {
    test_peer_fingerprint();
    test_exporter_capture_lifetime();
    return 0;
}
