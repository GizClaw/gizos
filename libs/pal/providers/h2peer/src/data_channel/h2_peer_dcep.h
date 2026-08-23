#ifndef H2_PEER_DCEP_H
#define H2_PEER_DCEP_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#define H2_PEER_DCEP_LABEL_MAX 128u

typedef struct h2_peer_dcep_open {
    int ordered;
    int reliable;
    const uint8_t *label;
    size_t label_len;
} h2_peer_dcep_open_t;

h2_pal_result_t h2_peer_dcep_write_open(
    const uint8_t *label,
    size_t label_len,
    int ordered,
    int reliable,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len);

h2_pal_result_t h2_peer_dcep_parse_open(
    const uint8_t *data,
    size_t len,
    h2_peer_dcep_open_t *out_open);

#endif
