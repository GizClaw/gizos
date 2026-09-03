#ifndef H2_BLOOMSPEAKER_PROTOCOL_H
#define H2_BLOOMSPEAKER_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H2_BLOOMSPEAKER_HANDSHAKE_SIZE 18u

void h2_bloomspeaker_handshake_make(
    uint8_t out[H2_BLOOMSPEAKER_HANDSHAKE_SIZE], uint64_t source,
    uint64_t target, uint32_t epoch);

bool h2_bloomspeaker_handshake_valid(const uint8_t *data, size_t size,
                                     uint64_t source, uint64_t target,
                                     uint32_t epoch);

#endif
