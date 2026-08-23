#ifndef H2_SCTP_TIMER_H
#define H2_SCTP_TIMER_H

#include "h2_sctp_internal.h"

h2_pal_result_t h2_sctp_timer_service(
    h2_pal_sctp_association_t *association,
    uint64_t now_ms,
    uint64_t *out_next_deadline_ms);

#endif
