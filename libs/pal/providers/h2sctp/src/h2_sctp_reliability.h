#ifndef H2_SCTP_RELIABILITY_H
#define H2_SCTP_RELIABILITY_H

#include "h2_sctp_internal.h"
#include "h2_sctp_wire.h"

h2_pal_result_t h2_sctp_reliability_handle_sack(
    h2_pal_sctp_association_t *association,
    const h2_sctp_chunk_view_t *chunk,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_reliability_send_pending(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_reliability_service(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *in_out_deadline_ms);
h2_pal_result_t h2_sctp_reliability_send_sack(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms);
h2_pal_result_t h2_sctp_reliability_note_data(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    bool immediate);
h2_pal_result_t h2_sctp_reliability_service_sack(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *in_out_deadline_ms);
void h2_sctp_reliability_acknowledge_through(
    h2_pal_sctp_association_t *association,
    uint32_t cumulative_tsn);
void h2_sctp_reliability_release_all(
    h2_pal_sctp_association_t *association);

#endif
