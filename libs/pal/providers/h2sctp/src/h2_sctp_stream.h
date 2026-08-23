#ifndef H2_SCTP_STREAM_H
#define H2_SCTP_STREAM_H

#include "h2_sctp_internal.h"
#include "h2_sctp_wire.h"

h2_sctp_stream_t *h2_sctp_stream_find(
    h2_pal_sctp_association_t *association,
    uint16_t stream_id);
h2_sctp_stream_t *h2_sctp_stream_get_or_create(
    h2_pal_sctp_association_t *association,
    uint16_t stream_id);
h2_pal_result_t h2_sctp_stream_queue_message(
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_message_t *message,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_stream_handle_data(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_stream_handle_forward_tsn(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_stream_handle_reconfig(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_stream_service(
    h2_pal_sctp_association_t *association);
void h2_sctp_stream_release_all(h2_pal_sctp_association_t *association);

#endif
