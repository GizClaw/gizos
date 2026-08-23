#include "h2_wolfssl_internal.h"

#include <limits.h>
#include <string.h>

#include <wolfssl/wolfio.h>

h2_pal_result_t h2_wolfssl_dtls_flush_pending(
    h2_pal_dtls_session_t *session) {
    while (session->pending_count > 0u) {
        size_t datagram_len = session->pending_lengths[0];
        h2_pal_result_t result = session->config.send(
            session->config.io_user, session->pending_bytes, datagram_len);
        if (result == H2_PAL_ERR_WOULD_BLOCK) {
            return result;
        }
        if (result != H2_PAL_OK) {
            session->terminal_error = result;
            return result;
        }
        session->pending_bytes_len -= datagram_len;
        --session->pending_count;
        if (session->pending_bytes_len > 0u) {
            memmove(
                session->pending_bytes,
                session->pending_bytes + datagram_len,
                session->pending_bytes_len);
        }
        if (session->pending_count > 0u) {
            memmove(
                session->pending_lengths,
                session->pending_lengths + 1,
                session->pending_count * sizeof(*session->pending_lengths));
        }
    }
    return H2_PAL_OK;
}

int h2_wolfssl_dtls_send(
    WOLFSSL *ssl, char *buffer, int length, void *context) {
    h2_pal_dtls_session_t *session = (h2_pal_dtls_session_t *)context;
    size_t datagram_len;
    h2_pal_result_t result;
    (void)ssl;

    if (session == NULL || buffer == NULL || length <= 0) {
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    datagram_len = (size_t)length;
    if (datagram_len > session->config.max_datagram_size ||
        datagram_len > session->config.max_pending_output_bytes -
                           session->pending_bytes_len ||
        session->pending_count >= session->config.max_pending_output_bytes) {
        session->terminal_error = H2_PAL_ERR_NO_SPACE;
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    memcpy(
        session->pending_bytes + session->pending_bytes_len,
        buffer, datagram_len);
    session->pending_lengths[session->pending_count++] = datagram_len;
    session->pending_bytes_len += datagram_len;

    result = h2_wolfssl_dtls_flush_pending(session);
    if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK) {
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    return length;
}

int h2_wolfssl_dtls_recv(
    WOLFSSL *ssl, char *buffer, int length, void *context) {
    h2_pal_dtls_session_t *session = (h2_pal_dtls_session_t *)context;
    (void)ssl;

    if (session == NULL || buffer == NULL || length <= 0) {
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    if (session->input == NULL || session->input_consumed) {
        return WOLFSSL_CBIO_ERR_WANT_READ;
    }
    if (session->input_len > (size_t)length ||
        session->input_len > (size_t)INT_MAX) {
        session->terminal_error = H2_PAL_ERR_TRUNCATED;
        return WOLFSSL_CBIO_ERR_GENERAL;
    }
    memcpy(buffer, session->input, session->input_len);
    session->input_consumed = 1;
    return (int)session->input_len;
}

void h2_wolfssl_dtls_set_input(
    h2_pal_dtls_session_t *session,
    const uint8_t *datagram,
    size_t datagram_len) {
    session->input = datagram;
    session->input_len = datagram_len;
    session->input_consumed = 0;
}

void h2_wolfssl_dtls_clear_input(h2_pal_dtls_session_t *session) {
    session->input = NULL;
    session->input_len = 0u;
    session->input_consumed = 0;
}
