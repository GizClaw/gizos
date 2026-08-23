#include "h2_sctp_internal.h"
#include "h2_sctp_test_peer.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void) {
    h2_sctp_test_endpoint_t endpoint;
    assert(h2_sctp_test_endpoint_init(
               &endpoint,
               H2_PAL_SCTP_ROLE_ACTIVE,
               5000u,
               5001u,
               256u,
               1024u,
               4096u,
               4096u) == H2_PAL_OK);
    endpoint.emit_would_block_count = 1u;
    assert(h2_pal_sctp_association_start(
               endpoint.api, endpoint.association, 10u) == H2_PAL_OK);
    uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
    assert(h2_pal_sctp_association_service(
               endpoint.api, endpoint.association, 10u, &deadline) ==
           H2_PAL_OK);
    assert(deadline == 1010u);
    assert(endpoint.packet_head != NULL);
    assert(h2_pal_sctp_association_service(
               endpoint.api, endpoint.association, 9u, &deadline) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(deadline == H2_PAL_SCTP_NO_DEADLINE);
    assert(h2_sctp_deadline_add(UINT64_MAX - 10u, 20u) == UINT64_MAX - 1u);
    h2_sctp_test_endpoint_deinit(&endpoint);

    assert(h2_sctp_test_endpoint_init(
               &endpoint,
               H2_PAL_SCTP_ROLE_ACTIVE,
               5000u,
               5001u,
               256u,
               1024u,
               4096u,
               4096u) == H2_PAL_OK);
    assert(h2_pal_sctp_association_start(
               endpoint.api, endpoint.association, 1u) == H2_PAL_OK);
    uint64_t now = 1u;
    for (unsigned attempt = 0u; attempt < 6u; ++attempt) {
        now += 60000u;
        (void)h2_pal_sctp_association_service(
            endpoint.api, endpoint.association, now, &deadline);
    }
    assert(endpoint.state == H2_PAL_SCTP_STATE_FAILED);
    assert(endpoint.state_reason == H2_PAL_ERR_TIMEOUT);
    h2_sctp_test_endpoint_deinit(&endpoint);
    return 0;
}
