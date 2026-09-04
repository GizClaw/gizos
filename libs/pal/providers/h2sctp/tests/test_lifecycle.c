#include "h2_sctp_internal.h"
#include "h2_sctp_test_peer.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void) {
    h2_sctp_test_endpoint_t endpoint;
    assert(h2_sctp_test_endpoint_init(
               &endpoint,
               H2_PAL_SCTP_ROLE_PASSIVE,
               5001u,
               5000u,
               256u,
               1024u,
               4096u,
               4096u) == H2_PAL_OK);
    const h2_sctp_config_t provider_config = {
        .mem = &endpoint.mem_api,
        .crypto = &endpoint.crypto_api,
    };
    h2_sctp_t *failed_provider = (h2_sctp_t *)(uintptr_t)1u;
    endpoint.fail_allocation_at = endpoint.allocation_count + 1u;
    assert(h2_sctp_create(&provider_config, &failed_provider) ==
           H2_PAL_ERR_NO_MEMORY);
    assert(failed_provider == NULL);
    endpoint.fail_allocation_at = 0u;
    endpoint.random_failure = H2_PAL_ERR_IO;
    h2_pal_sctp_association_t *failed_association =
        (h2_pal_sctp_association_t *)(uintptr_t)1u;
    assert(h2_pal_sctp_association_create(
               endpoint.api,
               &endpoint.association->config,
               &failed_association) == H2_PAL_ERR_IO);
    assert(failed_association == NULL);
    endpoint.random_failure = H2_PAL_OK;
    assert(h2_sctp_destroy(&endpoint.provider) == H2_PAL_ERR_INVALID_STATE);
    assert(endpoint.provider != NULL);
    endpoint.reenter_on_state = true;
    assert(h2_pal_sctp_association_start(
               endpoint.api, endpoint.association, 1u) == H2_PAL_OK);
    assert(endpoint.reentrant_result == H2_PAL_ERR_BUSY);
    assert(endpoint.reentrant_close_result == H2_PAL_ERR_BUSY);
    h2_sctp_test_endpoint_deinit(&endpoint);
    assert(endpoint.allocation_count ==
           endpoint.free_count + endpoint.allocation_failure_count);

    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
    assert(h2_pal_sctp_association_start(
               pair.passive.api, pair.passive.association, pair.now_ms) ==
           H2_PAL_OK);
    assert(h2_pal_sctp_association_start(
               pair.active.api, pair.active.association, pair.now_ms) ==
           H2_PAL_OK);
    /* An INIT that cannot be answered because every pooled packet buffer is
     * taken leaves the association untouched and the INIT undelivered. */
    uint8_t *held[H2_SCTP_DEFAULT_PACKET_POOL_SIZE];
    for (size_t index = 0u; index < H2_SCTP_DEFAULT_PACKET_POOL_SIZE; ++index) {
        held[index] = h2_sctp_packet_acquire(pair.passive.association);
        assert(held[index] != NULL);
    }
    assert(h2_sctp_test_transfer(
               &pair.active, &pair.passive, pair.now_ms) == 0u);
    assert(pair.active.packet_head != NULL);
    assert(pair.passive.association->peer_verification_tag == 0u);
    for (size_t index = 0u; index < H2_SCTP_DEFAULT_PACKET_POOL_SIZE; ++index) {
        h2_sctp_packet_release(pair.passive.association, held[index]);
    }
    assert(h2_sctp_test_transfer(
               &pair.active, &pair.passive, pair.now_ms) == 1u);
    pair.active.fail_allocation_at = pair.active.allocation_count + 1u;
    assert(h2_sctp_test_transfer(
               &pair.passive, &pair.active, pair.now_ms) == 0u);
    assert(pair.passive.packet_head != NULL);
    assert(pair.active.association->peer_verification_tag == 0u);
    pair.active.fail_allocation_at = 0u;
    (void)h2_sctp_test_pump(&pair, 32u);
    assert(pair.active.state == H2_PAL_SCTP_STATE_CONNECTED);
    assert(pair.passive.state == H2_PAL_SCTP_STATE_CONNECTED);
    h2_sctp_test_pair_deinit(&pair);

    assert(h2_sctp_test_pair_init(&pair, 256u, 1024u) == H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    pair.active.fail_allocation_at = pair.active.allocation_count + 1u;
    assert(h2_pal_sctp_association_shutdown(
               pair.active.api,
               pair.active.association,
               pair.now_ms) == H2_PAL_ERR_NO_MEMORY);
    assert(pair.active.state == H2_PAL_SCTP_STATE_CONNECTED);
    pair.active.fail_allocation_at = 0u;
    const uint32_t reset_sequence =
        pair.active.association->next_reset_sequence;
    pair.active.fail_allocation_at = pair.active.allocation_count + 2u;
    assert(h2_pal_sctp_association_reset_stream(
               pair.active.api,
               pair.active.association,
               4u,
               pair.now_ms) == H2_PAL_ERR_NO_MEMORY);
    assert(pair.active.association->next_reset_sequence == reset_sequence);
    assert(!pair.active.association->streams->reset_pending);
    pair.active.fail_allocation_at = 0u;
    const uint8_t data[300] = {0xa5u};
    const h2_pal_sctp_message_t message = {
        .data = data,
        .len = sizeof(data),
        .stream_id = 1u,
        .ppid = 53u,
        .reliability = H2_PAL_SCTP_RELIABILITY_RELIABLE,
    };
    const size_t send_used = pair.active.association->send_used;
    pair.active.fail_allocation_at = pair.active.allocation_count + 2u;
    assert(h2_pal_sctp_association_send_message(
               pair.active.api,
               pair.active.association,
               &message,
               pair.now_ms) == H2_PAL_ERR_NO_MEMORY);
    assert(pair.active.association->send_used == send_used);
    pair.active.fail_allocation_at = 0u;
    pair.active.emit_failure = H2_PAL_ERR_IO;
    assert(h2_pal_sctp_association_send_message(
               pair.active.api,
               pair.active.association,
               &message,
               pair.now_ms) == H2_PAL_ERR_IO);
    assert(pair.active.state == H2_PAL_SCTP_STATE_FAILED);
    assert(pair.active.state_reason == H2_PAL_ERR_IO);
    h2_sctp_test_pair_deinit(&pair);
    assert(pair.active.allocation_count ==
           pair.active.free_count + pair.active.allocation_failure_count);
    assert(pair.passive.allocation_count == pair.passive.free_count);
    return 0;
}
