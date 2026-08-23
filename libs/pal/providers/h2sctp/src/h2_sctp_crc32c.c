#include "h2_sctp_crc32c.h"

/* RFC 9260 Appendix B reflected CRC32C polynomial. */
#define H2_SCTP_CRC32C_POLYNOMIAL 0x82f63b78u

static uint32_t h2_sctp_crc32c_update(
    uint32_t crc,
    const uint8_t *data,
    size_t len) {
    for (size_t index = 0u; index < len; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (H2_SCTP_CRC32C_POLYNOMIAL & mask);
        }
    }
    return crc;
}

uint32_t h2_sctp_crc32c(const uint8_t *data, size_t len) {
    if (data == NULL && len != 0u) {
        return 0u;
    }
    return ~h2_sctp_crc32c_update(UINT32_MAX, data, len);
}

uint32_t h2_sctp_crc32c_packet(const uint8_t *packet, size_t len) {
    static const uint8_t zero_checksum[4] = {0u, 0u, 0u, 0u};
    if (packet == NULL || len < 12u) {
        return 0u;
    }
    uint32_t crc = h2_sctp_crc32c_update(UINT32_MAX, packet, 8u);
    crc = h2_sctp_crc32c_update(crc, zero_checksum, sizeof(zero_checksum));
    crc = h2_sctp_crc32c_update(crc, packet + 12u, len - 12u);
    return ~crc;
}
