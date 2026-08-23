#ifndef H2_SCTP_ASSOCIATION_H
#define H2_SCTP_ASSOCIATION_H

#include "h2_sctp_internal.h"

h2_pal_result_t h2_sctp_association_start_impl(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_association_input_impl(
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_association_shutdown_impl(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_association_send_shutdown(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_association_send_shutdown_ack(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_association_abort_impl(
    h2_pal_sctp_association_t *association,
    h2_pal_result_t reason,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_association_reset_stream_impl(
    h2_pal_sctp_association_t *association,
    uint16_t stream_id,
    uint64_t now_ms);

#endif
