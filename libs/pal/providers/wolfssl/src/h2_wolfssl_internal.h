#ifndef H2_WOLFSSL_INTERNAL_H
#define H2_WOLFSSL_INTERNAL_H

#include "h2_wolfssl.h"

#include <wolfssl/ssl.h>

struct h2_pal_dtls_session {
    h2_pal_dtls_session_config_t config;
    WOLFSSL_CTX *context;
    WOLFSSL *ssl;
    uint8_t local_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    uint8_t remote_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
    uint8_t *pending_bytes;
    size_t *pending_lengths;
    uint8_t *plaintext;
    size_t pending_bytes_len;
    size_t pending_count;
    const uint8_t *input;
    size_t input_len;
    uint64_t handshake_deadline_ms;
    uint64_t next_deadline_ms;
    h2_pal_result_t terminal_error;
    int input_consumed;
    int remote_fingerprint_set;
    int handshake_started;
    int handshake_complete;
    int close_started;
};

int h2_wolfssl_entropy_fill(uint8_t *out, size_t len);
int h2_wolfssl_generate_seed(unsigned char *out, unsigned int len);
int h2_wolfssl_strcasecmp(const char *first, const char *second);

void *h2_wolfssl_alloc(size_t len);
void *h2_wolfssl_realloc(void *ptr, size_t len);
void h2_wolfssl_free(void *ptr);

int h2_wolfssl_is_ready(void);
h2_pal_result_t h2_wolfssl_session_acquire(void);
void h2_wolfssl_session_release(void);

h2_pal_result_t h2_wolfssl_dtls_generate_identity(
    WOLFSSL_CTX *context,
    uint8_t out_fingerprint[H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE]);
h2_pal_result_t h2_wolfssl_dtls_verify_peer(
    h2_pal_dtls_session_t *session);

int h2_wolfssl_dtls_send(
    WOLFSSL *ssl, char *buffer, int length, void *context);
int h2_wolfssl_dtls_recv(
    WOLFSSL *ssl, char *buffer, int length, void *context);
h2_pal_result_t h2_wolfssl_dtls_flush_pending(
    h2_pal_dtls_session_t *session);
void h2_wolfssl_dtls_set_input(
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len);
void h2_wolfssl_dtls_clear_input(h2_pal_dtls_session_t *session);

#endif
