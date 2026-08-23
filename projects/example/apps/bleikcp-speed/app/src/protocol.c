#include "internal.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <string.h>

static uint16_t h2_speed_read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static uint32_t h2_speed_read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t h2_speed_read_u64(const uint8_t *p) {
    uint64_t value = 0u;
    for (unsigned i = 0u; i < 8u; ++i) {
        value |= (uint64_t)p[i] << (i * 8u);
    }
    return value;
}

static void h2_speed_write_u32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0u; i < 4u; ++i) {
        p[i] = (uint8_t)(value >> (i * 8u));
    }
}

static void h2_speed_write_u64(uint8_t *p, uint64_t value) {
    for (unsigned i = 0u; i < 8u; ++i) {
        p[i] = (uint8_t)(value >> (i * 8u));
    }
}

static uint8_t h2_speed_payload_byte(
    uint64_t session_id,
    uint8_t direction,
    uint64_t offset) {
    return (uint8_t)((session_id >> ((offset & 7u) * 8u)) ^
                     ((uint64_t)direction * 0x5au) ^ offset);
}

void h2_speed_fill_payload(
    uint8_t *out,
    size_t len,
    uint64_t session_id,
    uint8_t direction,
    uint64_t offset) {
    for (size_t i = 0u; i < len; ++i) {
        out[i] = h2_speed_payload_byte(session_id, direction, offset + i);
    }
}

int h2_speed_verify_payload(
    const uint8_t *data,
    size_t len,
    uint64_t session_id,
    uint8_t direction,
    uint64_t offset) {
    if (data == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t i = 0u; i < len; ++i) {
        if (data[i] != h2_speed_payload_byte(
                           session_id, direction, offset + i)) {
            return H2_PAL_ERR_FORMAT;
        }
    }
    return H2_PAL_OK;
}

void h2_speed_make_header(
    uint8_t out[H2_SPEED_HEADER_SIZE],
    bool response,
    uint64_t session_id,
    uint32_t chunk_size,
    int result) {
    memset(out, 0, H2_SPEED_HEADER_SIZE);
    memcpy(out, response ? "H2BR" : "H2BS", 4u);
    out[4] = H2_SPEED_PROTOCOL_VERSION;
    out[5] = response ? (uint8_t)(result == H2_PAL_OK ? 0u : 1u) : 0u;
    h2_speed_write_u64(out + 8u, session_id);
    h2_speed_write_u32(out + 16u, chunk_size);
    if (response) {
        h2_speed_write_u32(out + 20u, (uint32_t)result);
    }
}

int h2_speed_validate_header(
    const uint8_t header[H2_SPEED_HEADER_SIZE],
    bool response,
    uint64_t *out_session_id) {
    if (header == NULL || out_session_id == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (memcmp(header, response ? "H2BR" : "H2BS", 4u) != 0 ||
        header[4] != H2_SPEED_PROTOCOL_VERSION ||
        (!response && header[5] != 0u) ||
        h2_speed_read_u16(header + 6u) != 0u ||
        h2_speed_read_u32(header + 16u) != H2_SPEED_CHUNK_SIZE ||
        (!response && h2_speed_read_u32(header + 20u) != 0u) ||
        (response &&
         (header[5] != 0u || h2_speed_read_u32(header + 20u) != 0u))) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    *out_session_id = h2_speed_read_u64(header + 8u);
    return H2_PAL_OK;
}

uint32_t h2_speed_next_backoff_ms(uint32_t current_ms) {
    if (current_ms < 250u) {
        return 250u;
    }
    if (current_ms >= 2500u) {
        return 5000u;
    }
    return current_ms * 2u;
}

bool h2_speed_io_should_retry(int result) {
    return result == H2_PAL_ERR_WOULD_BLOCK;
}

double h2_speed_rate_kib_s(
    uint64_t current_bytes,
    uint64_t previous_bytes,
    uint64_t elapsed_ms) {
    if (elapsed_ms == 0u || current_bytes < previous_bytes) {
        return 0.0;
    }
    return (double)(current_bytes - previous_bytes) * 1000.0 /
           1024.0 / (double)elapsed_ms;
}
