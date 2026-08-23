#ifndef H2_BLEIKCP_SPEED_INTERNAL_H
#define H2_BLEIKCP_SPEED_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H2_SPEED_PROTOCOL_VERSION 1u
#define H2_SPEED_HEADER_SIZE 24u
#define H2_SPEED_CHUNK_SIZE 4096u

void h2_speed_fill_payload(
    uint8_t *out,
    size_t len,
    uint64_t session_id,
    uint8_t direction,
    uint64_t offset);

int h2_speed_verify_payload(
    const uint8_t *data,
    size_t len,
    uint64_t session_id,
    uint8_t direction,
    uint64_t offset);

void h2_speed_make_header(
    uint8_t out[H2_SPEED_HEADER_SIZE],
    bool response,
    uint64_t session_id,
    uint32_t chunk_size,
    int result);

int h2_speed_validate_header(
    const uint8_t header[H2_SPEED_HEADER_SIZE],
    bool response,
    uint64_t *out_session_id);

uint32_t h2_speed_next_backoff_ms(uint32_t current_ms);

bool h2_speed_io_should_retry(int result);

double h2_speed_rate_kib_s(
    uint64_t current_bytes,
    uint64_t previous_bytes,
    uint64_t elapsed_ms);

#endif
