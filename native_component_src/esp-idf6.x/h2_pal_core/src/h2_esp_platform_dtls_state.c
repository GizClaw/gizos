#include "h2_esp_platform_dtls_state.h"

#include <string.h>

void h2_esp_dtls_captured_state_reset_peer(
    h2_esp_dtls_captured_state_t *state) {
    if (state == NULL) {
        return;
    }
    memset(state->peer_fingerprint, 0, sizeof(state->peer_fingerprint));
    state->peer_fingerprint_set = 0;
}

h2_pal_result_t h2_esp_dtls_captured_state_set_peer(
    h2_esp_dtls_captured_state_t *state,
    const uint8_t *fingerprint,
    size_t fingerprint_len) {
    if (state == NULL || fingerprint == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (fingerprint_len != sizeof(state->peer_fingerprint)) {
        return H2_PAL_ERR_TRUNCATED;
    }
    memcpy(state->peer_fingerprint, fingerprint, fingerprint_len);
    state->peer_fingerprint_set = 1;
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_dtls_captured_state_verify_peer(
    const h2_esp_dtls_captured_state_t *state,
    const uint8_t expected[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]) {
    if (state == NULL || expected == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return state->peer_fingerprint_set &&
                   memcmp(state->peer_fingerprint, expected,
                          sizeof(state->peer_fingerprint)) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_TLS_VERIFY;
}

h2_pal_result_t h2_esp_dtls_captured_state_set_exporter(
    h2_esp_dtls_captured_state_t *state,
    const uint8_t *secret,
    size_t secret_len,
    const uint8_t client_random[32],
    const uint8_t server_random[32],
    int prf) {
    if (state == NULL || secret == NULL || client_random == NULL ||
        server_random == NULL || secret_len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (secret_len > sizeof(state->master_secret)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    memcpy(state->master_secret, secret, secret_len);
    state->master_secret_len = secret_len;
    memcpy(state->randoms, client_random, 32u);
    memcpy(state->randoms + 32u, server_random, 32u);
    state->prf = prf;
    state->exporter_ready = 1;
    return H2_PAL_OK;
}
