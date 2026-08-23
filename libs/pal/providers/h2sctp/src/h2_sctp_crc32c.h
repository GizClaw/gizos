#ifndef H2_SCTP_CRC32C_H
#define H2_SCTP_CRC32C_H

#include <stddef.h>
#include <stdint.h>

uint32_t h2_sctp_crc32c(const uint8_t *data, size_t len);
uint32_t h2_sctp_crc32c_packet(const uint8_t *packet, size_t len);

#endif
