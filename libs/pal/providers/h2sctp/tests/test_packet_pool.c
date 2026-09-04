#include "h2_sctp_internal.h"
#include "h2_sctp_reliability.h"
#include "h2_sctp_test_peer.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

/* The pool is one block from packet_mem, sized pool_size * max_packet_size,
 * created with the association and released with it; the default size
 * applies when the provider config says 0 and oversized requests clamp. */
static void pool_placement_and_sizing(void) {
    h2_sctp_test_endpoint_t endpoint;
    assert(h2_sctp_test_endpoint_init_with_pool(
               &endpoint, H2_PAL_SCTP_ROLE_ACTIVE, 5000u, 5001u, 256u, 1024u,
               4096u, 4096u, 3u) == H2_PAL_OK);
    assert(endpoint.packet_allocation_count == 1u);
    assert(endpoint.packet_allocation_bytes == 3u * 256u);
    assert(endpoint.association->packet_pool_size == 3u);
    h2_sctp_test_endpoint_deinit(&endpoint);
    assert(endpoint.packet_free_count == 1u);
    assert(endpoint.allocation_count ==
           endpoint.free_count + endpoint.allocation_failure_count);

    assert(h2_sctp_test_endpoint_init(
               &endpoint, H2_PAL_SCTP_ROLE_ACTIVE, 5000u, 5001u, 256u, 1024u,
               4096u, 4096u) == H2_PAL_OK);
    assert(endpoint.association->packet_pool_size ==
           H2_SCTP_DEFAULT_PACKET_POOL_SIZE);
    assert(endpoint.packet_allocation_bytes ==
           H2_SCTP_DEFAULT_PACKET_POOL_SIZE * 256u);
    h2_sctp_test_endpoint_deinit(&endpoint);

    assert(h2_sctp_test_endpoint_init_with_pool(
               &endpoint, H2_PAL_SCTP_ROLE_ACTIVE, 5000u, 5001u, 256u, 1024u,
               4096u, 4096u, H2_SCTP_MAX_PACKET_POOL_SIZE + 5u) == H2_PAL_OK);
    assert(endpoint.association->packet_pool_size ==
           H2_SCTP_MAX_PACKET_POOL_SIZE);
    h2_sctp_test_endpoint_deinit(&endpoint);
}

/* Acquire hands out each buffer once, exhaustion yields NULL, and a released
 * buffer is reused. Foreign or misaligned pointers are ignored on release. */
static void acquire_exhaust_and_reuse(void) {
    h2_sctp_test_endpoint_t endpoint;
    assert(h2_sctp_test_endpoint_init_with_pool(
               &endpoint, H2_PAL_SCTP_ROLE_ACTIVE, 5000u, 5001u, 256u, 1024u,
               4096u, 4096u, 3u) == H2_PAL_OK);
    h2_pal_sctp_association_t *association = endpoint.association;
    uint8_t *first = h2_sctp_packet_acquire(association);
    uint8_t *second = h2_sctp_packet_acquire(association);
    uint8_t *third = h2_sctp_packet_acquire(association);
    assert(first != NULL && second == first + 256u && third == first + 512u);
    assert(first == association->packet_pool);
    assert(h2_sctp_packet_acquire(association) == NULL);
    assert(association->packet_pool_used == 0x7u);

    uint8_t foreign[8];
    h2_sctp_packet_release(association, foreign);
    h2_sctp_packet_release(association, first + 1u);
    h2_sctp_packet_release(association, first + 3u * 256u);
    h2_sctp_packet_release(association, NULL);
    assert(association->packet_pool_used == 0x7u);

    h2_sctp_packet_release(association, second);
    assert(h2_sctp_packet_acquire(association) == second);
    h2_sctp_packet_release(association, first);
    h2_sctp_packet_release(association, second);
    h2_sctp_packet_release(association, third);
    assert(association->packet_pool_used == 0u);
    // A double release is harmless.
    h2_sctp_packet_release(association, third);
    assert(association->packet_pool_used == 0u);
    h2_sctp_test_endpoint_deinit(&endpoint);
    assert(endpoint.allocation_count ==
           endpoint.free_count + endpoint.allocation_failure_count);
}

static size_t count_tx_fragments(const h2_pal_sctp_association_t *association,
                                 bool sent) {
    size_t count = 0u;
    for (const h2_sctp_tx_fragment_t *fragment = association->tx_fragments;
         fragment != NULL; fragment = fragment->next) {
        if (fragment->sent == sent) {
            ++count;
        }
    }
    return count;
}

/* With every buffer taken, a control emit reports NO_MEMORY and a DATA send
 * reports WOULD_BLOCK, both without side effects: no pending packet, no
 * control copy, DATA left unsent and unassigned. Once a buffer returns, the
 * queued data goes out on the next service call. */
static void exhaustion_reports_would_block_then_recovers(void) {
    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init_with_pool(&pair, 256u, 1024u, 2u) ==
           H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    h2_pal_sctp_association_t *association = pair.active.association;
    uint8_t *first = h2_sctp_packet_acquire(association);
    uint8_t *second = h2_sctp_packet_acquire(association);
    assert(first != NULL && second != NULL);

    static const uint8_t heartbeat[16] = {H2_SCTP_CHUNK_HEARTBEAT, 0u, 0u, 16u,
                                          0u, 1u, 0u, 12u};
    assert(h2_sctp_emit_chunks(association, association->peer_verification_tag,
                               heartbeat, sizeof(heartbeat),
                               H2_SCTP_CONTROL_NONE,
                               pair.now_ms) == H2_PAL_ERR_NO_MEMORY);
    assert(association->pending_emit == NULL);
    assert(h2_sctp_emit_chunks(association, association->peer_verification_tag,
                               heartbeat, sizeof(heartbeat),
                               H2_SCTP_CONTROL_SHUTDOWN,
                               pair.now_ms) == H2_PAL_ERR_NO_MEMORY);
    assert(association->control_packet == NULL &&
           association->control_kind == H2_SCTP_CONTROL_NONE);
    assert(pair.active.packet_head == NULL);

    const uint8_t payload[] = "pooled";
    const h2_pal_sctp_message_t message = {
        .data = payload,
        .len = sizeof(payload),
        .stream_id = 1u,
        .ppid = 51u,
    };
    assert(h2_pal_sctp_association_send_message(
               pair.active.api, association, &message, pair.now_ms) ==
           H2_PAL_OK);
    assert(count_tx_fragments(association, false) == 1u);
    assert(!association->tx_fragments->tsn_assigned);
    assert(association->flight_size == 0u);
    assert(pair.active.packet_head == NULL);

    h2_sctp_packet_release(association, first);
    h2_sctp_packet_release(association, second);
    uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
    pair.now_ms += 1u;
    assert(h2_pal_sctp_association_service(
               pair.active.api, association, pair.now_ms, &deadline) ==
           H2_PAL_OK);
    assert(count_tx_fragments(association, true) == 1u);
    assert(pair.active.packet_head != NULL);
    (void)h2_sctp_test_pump(&pair, 16u);
    assert(pair.passive.message_count == 1u);
    assert(pair.passive.messages[0].len == sizeof(payload));
    assert(memcmp(pair.passive.messages[0].data, payload, sizeof(payload)) ==
           0);
    assert(association->packet_pool_used == 0u);
    h2_sctp_test_pair_deinit(&pair);
}

/* A transport WOULD_BLOCK keeps the pooled packet as pending_emit (one
 * buffer stays taken), the retry releases it, and the pool never leaks
 * across a connect, bulk transfer and close. */
static void pending_emit_holds_one_buffer(void) {
    h2_sctp_test_pair_t pair;
    assert(h2_sctp_test_pair_init_with_pool(&pair, 256u, 1024u, 1u) ==
           H2_PAL_OK);
    assert(h2_sctp_test_connect(&pair));
    h2_pal_sctp_association_t *association = pair.active.association;
    assert(association->packet_pool_used == 0u);

    uint8_t payload[600];
    memset(payload, 0x5a, sizeof(payload));
    const h2_pal_sctp_message_t message = {
        .data = payload,
        .len = sizeof(payload),
        .stream_id = 2u,
        .ppid = 53u,
    };
    pair.active.emit_would_block_count = 1u;
    assert(h2_pal_sctp_association_send_message(
               pair.active.api, association, &message, pair.now_ms) ==
           H2_PAL_OK);
    assert(association->pending_emit != NULL);
    assert(association->pending_emit == association->packet_pool);
    assert(association->packet_pool_used == 0x1u);
    assert(h2_sctp_packet_acquire(association) == NULL);
    // Fragments behind the pending packet wait; none was falsely sent.
    assert(count_tx_fragments(association, true) == 1u);
    assert(count_tx_fragments(association, false) >= 1u);

    (void)h2_sctp_test_pump(&pair, 32u);
    assert(association->pending_emit == NULL);
    assert(association->packet_pool_used == 0u);
    assert(pair.passive.message_count == 1u);
    assert(pair.passive.messages[0].len == sizeof(payload));
    assert(memcmp(pair.passive.messages[0].data, payload, sizeof(payload)) ==
           0);
    assert(pair.active.packet_allocation_count == 1u);
    h2_sctp_test_pair_deinit(&pair);
    assert(pair.active.packet_free_count == 1u);
    assert(pair.active.allocation_count ==
           pair.active.free_count + pair.active.allocation_failure_count);
}

/* Association creation fails cleanly when the pool cannot be allocated. */
static void pool_allocation_failure(void) {
    h2_sctp_test_endpoint_t endpoint;
    assert(h2_sctp_test_endpoint_init(
               &endpoint, H2_PAL_SCTP_ROLE_ACTIVE, 5000u, 5001u, 256u, 1024u,
               4096u, 4096u) == H2_PAL_OK);
    const h2_pal_sctp_association_config_t oversized = {
        .role = H2_PAL_SCTP_ROLE_ACTIVE,
        .local_port = 5000u,
        .remote_port = 5001u,
        .inbound_streams = 1u,
        .outbound_streams = 1u,
        .max_packet_size = SIZE_MAX,
        .max_message_size = 1024u,
        .send_buffer_size = 4096u,
        .receive_buffer_size = 4096u,
        .callbacks = endpoint.association->config.callbacks,
    };
    h2_pal_sctp_association_t *created =
        (h2_pal_sctp_association_t *)(uintptr_t)1u;
    assert(h2_pal_sctp_association_create(endpoint.api, &oversized, &created) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(created == NULL);
    h2_sctp_test_endpoint_deinit(&endpoint);
    assert(endpoint.allocation_count ==
           endpoint.free_count + endpoint.allocation_failure_count);
}

int main(void) {
    pool_placement_and_sizing();
    acquire_exhaust_and_reuse();
    exhaustion_reports_would_block_then_recovers();
    pending_emit_holds_one_buffer();
    pool_allocation_failure();
    return 0;
}
