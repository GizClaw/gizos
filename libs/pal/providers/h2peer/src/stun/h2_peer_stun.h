#ifndef H2_PEER_STUN_H
#define H2_PEER_STUN_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#define H2_PEER_STUN_HEADER_SIZE 20u
#define H2_PEER_STUN_ATTRIBUTE_MAX 512u

typedef struct h2_peer_stun_message {
    uint16_t type;
    uint8_t transaction_id[12];
    size_t attribute_count;
} h2_peer_stun_message_t;

h2_pal_result_t h2_peer_stun_parse(
    const uint8_t *data,
    size_t len,
    h2_peer_stun_message_t *out_message);

h2_pal_result_t h2_peer_stun_write_binding_request(
    const uint8_t transaction_id[12],
    uint8_t *out,
    size_t out_cap,
    size_t *out_len);

#endif
