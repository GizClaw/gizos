#ifndef H2_ESP_PLATFORM_DTLS_STATE_H
#define H2_ESP_PLATFORM_DTLS_STATE_H

#include "h2/pal/net/h2_pal_dtls.h"

#include <stddef.h>
#include <stdint.h>

#define H2_ESP_DTLS_MASTER_SECRET_CAPACITY 48u
#define H2_ESP_DTLS_RANDOMS_SIZE 64u

typedef struct h2_esp_dtls_captured_state {
    uint8_t peer_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    uint8_t master_secret[H2_ESP_DTLS_MASTER_SECRET_CAPACITY];
    uint8_t randoms[H2_ESP_DTLS_RANDOMS_SIZE];
    size_t master_secret_len;
    int prf;
    int peer_fingerprint_set;
    int exporter_ready;
} h2_esp_dtls_captured_state_t;

void h2_esp_dtls_captured_state_reset_peer(
    h2_esp_dtls_captured_state_t *state);

h2_pal_result_t h2_esp_dtls_captured_state_set_peer(
    h2_esp_dtls_captured_state_t *state,
    const uint8_t *fingerprint,
    size_t fingerprint_len);

h2_pal_result_t h2_esp_dtls_captured_state_verify_peer(
    const h2_esp_dtls_captured_state_t *state,
    const uint8_t expected[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]);

h2_pal_result_t h2_esp_dtls_captured_state_set_exporter(
    h2_esp_dtls_captured_state_t *state,
    const uint8_t *secret,
    size_t secret_len,
    const uint8_t client_random[32],
    const uint8_t server_random[32],
    int prf);

#endif
