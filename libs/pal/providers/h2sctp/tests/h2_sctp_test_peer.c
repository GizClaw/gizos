#include "h2_sctp_test_peer.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void *h2_sctp_test_alloc(void *user, size_t size) {
    h2_sctp_test_endpoint_t *endpoint = (h2_sctp_test_endpoint_t *)user;
    endpoint->allocation_count++;
    if (endpoint->fail_allocation_at != 0u &&
        endpoint->allocation_count == endpoint->fail_allocation_at) {
        endpoint->allocation_failure_count++;
        return NULL;
    }
    return malloc(size);
}

static void *h2_sctp_test_realloc(void *user, void *pointer, size_t size) {
    h2_sctp_test_endpoint_t *endpoint = (h2_sctp_test_endpoint_t *)user;
    endpoint->allocation_count++;
    if (endpoint->fail_allocation_at != 0u &&
        endpoint->allocation_count == endpoint->fail_allocation_at) {
        endpoint->allocation_failure_count++;
        return NULL;
    }
    return realloc(pointer, size);
}

static void h2_sctp_test_free(void *user, void *pointer) {
    h2_sctp_test_endpoint_t *endpoint = (h2_sctp_test_endpoint_t *)user;
    if (pointer != NULL) {
        endpoint->free_count++;
        free(pointer);
    }
}

static h2_pal_result_t h2_sctp_test_random(void *user, uint8_t *out,
                                           size_t len) {
    h2_sctp_test_endpoint_t *endpoint = (h2_sctp_test_endpoint_t *)user;
    if (out == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (endpoint->random_failure != H2_PAL_OK) {
        return endpoint->random_failure;
    }
    for (size_t index = 0u; index < len; ++index) {
        uint32_t value = endpoint->random_state;
        value ^= value << 13u;
        value ^= value >> 17u;
        value ^= value << 5u;
        endpoint->random_state = value == 0u ? 0x6d2b79f5u : value;
        out[index] = (uint8_t)endpoint->random_state;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t h2_sctp_test_emit(
    void *user,
    h2_pal_sctp_association_t *association,
    const uint8_t *packet,
    size_t packet_len) {
    h2_sctp_test_endpoint_t *endpoint = (h2_sctp_test_endpoint_t *)user;
    (void)association;
    if (endpoint->emit_would_block_count != 0u) {
        endpoint->emit_would_block_count--;
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (endpoint->emit_failure != H2_PAL_OK) {
        return endpoint->emit_failure;
    }
    h2_sctp_test_packet_t *queued = malloc(sizeof(*queued));
    if (queued == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    queued->data = malloc(packet_len);
    if (queued->data == NULL) {
        free(queued);
        return H2_PAL_ERR_NO_MEMORY;
    }
    memcpy(queued->data, packet, packet_len);
    queued->len = packet_len;
    queued->next = NULL;
    if (endpoint->packet_tail == NULL) {
        endpoint->packet_head = queued;
    } else {
        endpoint->packet_tail->next = queued;
    }
    endpoint->packet_tail = queued;
    return H2_PAL_OK;
}

static void h2_sctp_test_state(
    void *user,
    h2_pal_sctp_association_t *association,
    h2_pal_sctp_state_t state,
    h2_pal_result_t reason) {
    h2_sctp_test_endpoint_t *endpoint = (h2_sctp_test_endpoint_t *)user;
    (void)association;
    endpoint->state = state;
    endpoint->state_reason = reason;
    endpoint->state_events++;
    if (endpoint->reenter_on_state) {
        uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
        endpoint->reentrant_result = h2_pal_sctp_association_service(
            endpoint->api, association, 1u, &deadline);
        h2_pal_sctp_association_t *reentrant_association = association;
        endpoint->reentrant_close_result = h2_pal_sctp_association_close(
            endpoint->api, &reentrant_association);
        assert(reentrant_association == association);
    }
}

static h2_pal_result_t h2_sctp_test_message(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_received_message_t *message) {
    h2_sctp_test_endpoint_t *endpoint = (h2_sctp_test_endpoint_t *)user;
    (void)association;
    if (endpoint->message_would_block_count != 0u) {
        endpoint->message_would_block_count--;
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    assert(endpoint->message_count < H2_SCTP_TEST_MAX_MESSAGES);
    h2_sctp_test_message_t *stored =
        &endpoint->messages[endpoint->message_count++];
    stored->data = malloc(message->len);
    assert(stored->data != NULL);
    memcpy(stored->data, message->data, message->len);
    stored->len = message->len;
    stored->stream_id = message->stream_id;
    stored->ppid = message->ppid;
    stored->unordered = message->unordered;
    return H2_PAL_OK;
}

static void h2_sctp_test_reset(
    void *user,
    h2_pal_sctp_association_t *association,
    const h2_pal_sctp_stream_reset_event_t *event) {
    h2_sctp_test_endpoint_t *endpoint = (h2_sctp_test_endpoint_t *)user;
    (void)association;
    assert(event->result == H2_PAL_OK);
    if (event->direction == H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED) {
        endpoint->outgoing_reset_events++;
    } else {
        endpoint->incoming_reset_events++;
    }
}

static void *h2_sctp_test_packet_alloc(void *user, size_t size) {
    h2_sctp_test_endpoint_t *endpoint = (h2_sctp_test_endpoint_t *)user;
    endpoint->packet_allocation_count++;
    endpoint->packet_allocation_bytes += size;
    return malloc(size);
}

static void h2_sctp_test_packet_free(void *user, void *pointer) {
    h2_sctp_test_endpoint_t *endpoint = (h2_sctp_test_endpoint_t *)user;
    if (pointer != NULL) {
        endpoint->packet_free_count++;
        free(pointer);
    }
}

static const h2_pal_mem_vtable_t h2_sctp_test_packet_mem_vtable = {
    .alloc = h2_sctp_test_packet_alloc,
    .free = h2_sctp_test_packet_free,
};

static const h2_pal_mem_vtable_t h2_sctp_test_mem_vtable = {
    .alloc = h2_sctp_test_alloc,
    .realloc = h2_sctp_test_realloc,
    .free = h2_sctp_test_free,
};

static const h2_pal_crypto_vtable_t h2_sctp_test_crypto_vtable = {
    .random = h2_sctp_test_random,
};

h2_pal_result_t h2_sctp_test_endpoint_init(
    h2_sctp_test_endpoint_t *endpoint,
    h2_pal_sctp_role_t role,
    uint16_t local_port,
    uint16_t remote_port,
    size_t max_packet_size,
    size_t max_message_size,
    size_t send_buffer_size,
    size_t receive_buffer_size) {
    return h2_sctp_test_endpoint_init_with_pool(
        endpoint,
        role,
        local_port,
        remote_port,
        max_packet_size,
        max_message_size,
        send_buffer_size,
        receive_buffer_size,
        0u);
}

h2_pal_result_t h2_sctp_test_endpoint_init_with_pool(
    h2_sctp_test_endpoint_t *endpoint,
    h2_pal_sctp_role_t role,
    uint16_t local_port,
    uint16_t remote_port,
    size_t max_packet_size,
    size_t max_message_size,
    size_t send_buffer_size,
    size_t receive_buffer_size,
    size_t packet_pool_size) {
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->random_state = role == H2_PAL_SCTP_ROLE_ACTIVE
                                 ? 0x12345678u
                                 : 0x87654321u;
    endpoint->state = H2_PAL_SCTP_STATE_NEW;
    endpoint->mem_api.user = endpoint;
    endpoint->mem_api.vtable = &h2_sctp_test_mem_vtable;
    endpoint->packet_mem_api.user = endpoint;
    endpoint->packet_mem_api.vtable = &h2_sctp_test_packet_mem_vtable;
    endpoint->crypto_api.user = endpoint;
    endpoint->crypto_api.vtable = &h2_sctp_test_crypto_vtable;
    const h2_sctp_config_t provider_config = {
        .mem = &endpoint->mem_api,
        .crypto = &endpoint->crypto_api,
        .packet_mem = &endpoint->packet_mem_api,
        .packet_pool_size = packet_pool_size,
    };
    h2_pal_result_t result = h2_sctp_create(
        &provider_config, &endpoint->provider);
    if (result != H2_PAL_OK) {
        return result;
    }
    endpoint->api = h2_sctp_api(endpoint->provider);
    const h2_pal_sctp_association_config_t association_config = {
        .role = role,
        .local_port = local_port,
        .remote_port = remote_port,
        .inbound_streams = 16u,
        .outbound_streams = 16u,
        .max_packet_size = max_packet_size,
        .max_message_size = max_message_size,
        .send_buffer_size = send_buffer_size,
        .receive_buffer_size = receive_buffer_size,
        .cookie_lifetime_ms = 60000u,
        .callbacks = {
            .user = endpoint,
            .emit_packet = h2_sctp_test_emit,
            .on_state = h2_sctp_test_state,
            .on_message = h2_sctp_test_message,
            .on_stream_reset = h2_sctp_test_reset,
        },
    };
    result = h2_pal_sctp_association_create(
        endpoint->api, &association_config, &endpoint->association);
    if (result != H2_PAL_OK) {
        (void)h2_sctp_destroy(&endpoint->provider);
    }
    return result;
}

static void h2_sctp_test_free_packets(h2_sctp_test_endpoint_t *endpoint) {
    while (endpoint->packet_head != NULL) {
        h2_sctp_test_packet_t *packet = endpoint->packet_head;
        endpoint->packet_head = packet->next;
        free(packet->data);
        free(packet);
    }
    endpoint->packet_tail = NULL;
}

void h2_sctp_test_endpoint_deinit(h2_sctp_test_endpoint_t *endpoint) {
    h2_sctp_test_free_packets(endpoint);
    for (size_t index = 0u; index < endpoint->message_count; ++index) {
        free(endpoint->messages[index].data);
        endpoint->messages[index].data = NULL;
    }
    endpoint->message_count = 0u;
    if (endpoint->association != NULL) {
        assert(h2_pal_sctp_association_close(
                   endpoint->api, &endpoint->association) == H2_PAL_OK);
    }
    if (endpoint->provider != NULL) {
        assert(h2_sctp_destroy(&endpoint->provider) == H2_PAL_OK);
    }
}

h2_pal_result_t h2_sctp_test_pair_init(
    h2_sctp_test_pair_t *pair,
    size_t max_packet_size,
    size_t max_message_size) {
    return h2_sctp_test_pair_init_with_pool(
        pair, max_packet_size, max_message_size, 0u);
}

h2_pal_result_t h2_sctp_test_pair_init_with_pool(
    h2_sctp_test_pair_t *pair,
    size_t max_packet_size,
    size_t max_message_size,
    size_t packet_pool_size) {
    memset(pair, 0, sizeof(*pair));
    pair->now_ms = 1u;
    if (max_message_size > (SIZE_MAX - max_packet_size) / 4u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    const size_t buffer_size = max_message_size * 4u + max_packet_size;
    h2_pal_result_t result = h2_sctp_test_endpoint_init_with_pool(
        &pair->active,
        H2_PAL_SCTP_ROLE_ACTIVE,
        5000u,
        5001u,
        max_packet_size,
        max_message_size,
        buffer_size,
        buffer_size,
        packet_pool_size);
    if (result != H2_PAL_OK) {
        return result;
    }
    result = h2_sctp_test_endpoint_init_with_pool(
        &pair->passive,
        H2_PAL_SCTP_ROLE_PASSIVE,
        5001u,
        5000u,
        max_packet_size,
        max_message_size,
        buffer_size,
        buffer_size,
        packet_pool_size);
    if (result != H2_PAL_OK) {
        h2_sctp_test_endpoint_deinit(&pair->active);
    }
    return result;
}

void h2_sctp_test_pair_deinit(h2_sctp_test_pair_t *pair) {
    h2_sctp_test_endpoint_deinit(&pair->active);
    h2_sctp_test_endpoint_deinit(&pair->passive);
}

static void h2_sctp_test_reorder(h2_sctp_test_endpoint_t *endpoint) {
    h2_sctp_test_packet_t *first = endpoint->packet_head;
    if (!endpoint->reorder_next || first == NULL || first->next == NULL) {
        return;
    }
    h2_sctp_test_packet_t *second = first->next;
    first->next = second->next;
    second->next = first;
    endpoint->packet_head = second;
    if (endpoint->packet_tail == second) {
        endpoint->packet_tail = first;
    }
    endpoint->reorder_next = false;
}

size_t h2_sctp_test_transfer(
    h2_sctp_test_endpoint_t *from,
    h2_sctp_test_endpoint_t *to,
    uint64_t now_ms) {
    h2_sctp_test_reorder(from);
    size_t transferred = 0u;
    while (from->packet_head != NULL) {
        h2_sctp_test_packet_t *packet = from->packet_head;
        if (from->drop_next != 0u) {
            from->drop_next--;
        } else {
            h2_pal_result_t result = h2_pal_sctp_association_input_packet(
                to->api,
                to->association,
                packet->data,
                packet->len,
                now_ms);
            if (result == H2_PAL_ERR_WOULD_BLOCK) {
                break;
            }
            assert(result == H2_PAL_OK);
            transferred++;
            if (from->duplicate_next != 0u) {
                from->duplicate_next--;
                assert(h2_pal_sctp_association_input_packet(
                           to->api,
                           to->association,
                           packet->data,
                           packet->len,
                           now_ms) == H2_PAL_OK);
            }
        }
        from->packet_head = packet->next;
        if (from->packet_head == NULL) {
            from->packet_tail = NULL;
        }
        free(packet->data);
        free(packet);
    }
    return transferred;
}

size_t h2_sctp_test_pump(h2_sctp_test_pair_t *pair, unsigned rounds) {
    size_t total = 0u;
    for (unsigned round = 0u; round < rounds; ++round) {
        const size_t moved =
            h2_sctp_test_transfer(&pair->active, &pair->passive, pair->now_ms) +
            h2_sctp_test_transfer(&pair->passive, &pair->active, pair->now_ms);
        total += moved;
        uint64_t deadline = H2_PAL_SCTP_NO_DEADLINE;
        assert(h2_pal_sctp_association_service(
                   pair->active.api,
                   pair->active.association,
                   pair->now_ms,
                   &deadline) == H2_PAL_OK);
        assert(h2_pal_sctp_association_service(
                   pair->passive.api,
                   pair->passive.association,
                   pair->now_ms,
                   &deadline) == H2_PAL_OK);
        if (moved == 0u && pair->active.packet_head == NULL &&
            pair->passive.packet_head == NULL) {
            break;
        }
    }
    return total;
}

bool h2_sctp_test_connect(h2_sctp_test_pair_t *pair) {
    if (h2_pal_sctp_association_start(
            pair->passive.api, pair->passive.association, pair->now_ms) !=
        H2_PAL_OK) {
        return false;
    }
    if (h2_pal_sctp_association_start(
            pair->active.api, pair->active.association, pair->now_ms) !=
        H2_PAL_OK) {
        return false;
    }
    (void)h2_sctp_test_pump(pair, 32u);
    return pair->active.state == H2_PAL_SCTP_STATE_CONNECTED &&
           pair->passive.state == H2_PAL_SCTP_STATE_CONNECTED;
}
