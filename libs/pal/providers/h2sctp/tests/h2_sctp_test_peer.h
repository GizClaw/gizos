#ifndef H2_SCTP_TEST_PEER_H
#define H2_SCTP_TEST_PEER_H

#include "h2_sctp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H2_SCTP_TEST_MAX_MESSAGES 192u

typedef struct h2_sctp_test_packet {
    uint8_t *data;
    size_t len;
    struct h2_sctp_test_packet *next;
} h2_sctp_test_packet_t;

typedef struct h2_sctp_test_message {
    uint8_t *data;
    size_t len;
    uint16_t stream_id;
    uint32_t ppid;
    bool unordered;
} h2_sctp_test_message_t;

typedef struct h2_sctp_test_endpoint {
    h2_pal_mem_api_t mem_api;
    /* Separate allocator for the provider's packet pool so tests can see
     * where and how large the pool is. */
    h2_pal_mem_api_t packet_mem_api;
    unsigned packet_allocation_count;
    unsigned packet_free_count;
    size_t packet_allocation_bytes;
    h2_pal_crypto_api_t crypto_api;
    h2_sctp_t *provider;
    const h2_pal_sctp_api_t *api;
    h2_pal_sctp_association_t *association;
    h2_sctp_test_packet_t *packet_head;
    h2_sctp_test_packet_t *packet_tail;
    h2_sctp_test_message_t messages[H2_SCTP_TEST_MAX_MESSAGES];
    size_t message_count;
    h2_pal_sctp_state_t state;
    h2_pal_result_t state_reason;
    unsigned state_events;
    unsigned outgoing_reset_events;
    unsigned incoming_reset_events;
    unsigned allocation_count;
    unsigned allocation_failure_count;
    unsigned free_count;
    unsigned fail_allocation_at;
    unsigned emit_would_block_count;
    unsigned message_would_block_count;
    h2_pal_result_t emit_failure;
    bool reenter_on_state;
    h2_pal_result_t reentrant_result;
    h2_pal_result_t reentrant_close_result;
    unsigned drop_next;
    unsigned duplicate_next;
    bool reorder_next;
    uint32_t random_state;
    h2_pal_result_t random_failure;
} h2_sctp_test_endpoint_t;

typedef struct h2_sctp_test_pair {
    h2_sctp_test_endpoint_t active;
    h2_sctp_test_endpoint_t passive;
    uint64_t now_ms;
} h2_sctp_test_pair_t;

h2_pal_result_t h2_sctp_test_endpoint_init(
    h2_sctp_test_endpoint_t *endpoint,
    h2_pal_sctp_role_t role,
    uint16_t local_port,
    uint16_t remote_port,
    size_t max_packet_size,
    size_t max_message_size,
    size_t send_buffer_size,
    size_t receive_buffer_size);
/* As above, with an explicit provider packet pool size (0 = default). */
h2_pal_result_t h2_sctp_test_endpoint_init_with_pool(
    h2_sctp_test_endpoint_t *endpoint,
    h2_pal_sctp_role_t role,
    uint16_t local_port,
    uint16_t remote_port,
    size_t max_packet_size,
    size_t max_message_size,
    size_t send_buffer_size,
    size_t receive_buffer_size,
    size_t packet_pool_size);
void h2_sctp_test_endpoint_deinit(h2_sctp_test_endpoint_t *endpoint);

h2_pal_result_t h2_sctp_test_pair_init(
    h2_sctp_test_pair_t *pair,
    size_t max_packet_size,
    size_t max_message_size);
h2_pal_result_t h2_sctp_test_pair_init_with_pool(
    h2_sctp_test_pair_t *pair,
    size_t max_packet_size,
    size_t max_message_size,
    size_t packet_pool_size);
void h2_sctp_test_pair_deinit(h2_sctp_test_pair_t *pair);
bool h2_sctp_test_connect(h2_sctp_test_pair_t *pair);
size_t h2_sctp_test_pump(h2_sctp_test_pair_t *pair, unsigned rounds);
size_t h2_sctp_test_transfer(
    h2_sctp_test_endpoint_t *from,
    h2_sctp_test_endpoint_t *to,
    uint64_t now_ms);

#endif
