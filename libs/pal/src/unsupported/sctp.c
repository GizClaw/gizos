#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_sctp_association_create(
    void *user,
    const h2_pal_sctp_association_config_t *config,
    h2_pal_sctp_association_t **out_association) {
    (void)user;
    (void)config;
    if (out_association != NULL) {
        *out_association = NULL;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sctp_association_start(
    void *user,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    (void)user;
    (void)association;
    (void)now_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sctp_association_input_packet(
    void *user,
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len,
    uint64_t now_ms) {
    (void)user;
    (void)association;
    (void)packet;
    (void)packet_len;
    (void)now_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sctp_association_service(
    void *user,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *out_next_deadline_ms) {
    (void)user;
    (void)association;
    (void)now_ms;
    if (out_next_deadline_ms != NULL) {
        *out_next_deadline_ms = H2_PAL_SCTP_NO_DEADLINE;
    }
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sctp_association_send_message(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_message_t *message,
    uint64_t now_ms) {
    (void)user;
    (void)association;
    (void)message;
    (void)now_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sctp_association_is_writable(
    void *user,
    h2_pal_sctp_association_t *association,
    bool *out_writable) {
    (void)user;
    (void)association;
    *out_writable = false;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sctp_association_reset_stream(
    void *user,
    h2_pal_sctp_association_t *association,
    uint16_t stream_id,
    uint64_t now_ms) {
    (void)user;
    (void)association;
    (void)stream_id;
    (void)now_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sctp_association_shutdown(
    void *user,
    h2_pal_sctp_association_t *association,
    uint64_t now_ms) {
    (void)user;
    (void)association;
    (void)now_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sctp_association_abort(
    void *user,
    h2_pal_sctp_association_t *association,
    h2_pal_result_t reason,
    uint64_t now_ms) {
    (void)user;
    (void)association;
    (void)reason;
    (void)now_ms;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_sctp_association_close(
    void *user,
    h2_pal_sctp_association_t **association) {
    (void)user;
    (void)association;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_sctp_vtable_t unsupported_sctp_vtable = {
    .association_create = unsupported_sctp_association_create,
    .association_start = unsupported_sctp_association_start,
    .association_input_packet = unsupported_sctp_association_input_packet,
    .association_service = unsupported_sctp_association_service,
    .association_send_message = unsupported_sctp_association_send_message,
    .association_is_writable = unsupported_sctp_association_is_writable,
    .association_reset_stream = unsupported_sctp_association_reset_stream,
    .association_shutdown = unsupported_sctp_association_shutdown,
    .association_abort = unsupported_sctp_association_abort,
    .association_close = unsupported_sctp_association_close,
};

static const h2_pal_sctp_api_t unsupported_sctp_api = {
    .user = NULL,
    .vtable = &unsupported_sctp_vtable,
};

const h2_pal_sctp_api_t *h2_pal_unsupported_sctp_api(void) {
    return &unsupported_sctp_api;
}
