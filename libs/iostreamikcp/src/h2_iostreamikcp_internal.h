#ifndef H2_IOSTREAMIKCP_INTERNAL_H
#define H2_IOSTREAMIKCP_INTERNAL_H

#include "h2_iostreamikcp.h"

#include "ikcp.h"

#include <stdint.h>

#define H2_IOSTREAMIKCP_FRAME_MAGIC_LEN 6u
#define H2_IOSTREAMIKCP_FRAME_LEN_OFFSET 12u

extern const uint8_t h2_iostreamikcp_frame_magic[H2_IOSTREAMIKCP_FRAME_MAGIC_LEN];

uint32_t h2_iostreamikcp_crc32(const uint8_t *data, size_t len);
uint16_t h2_iostreamikcp_read_le16(const uint8_t *data);
uint32_t h2_iostreamikcp_read_le32(const uint8_t *data);
void h2_iostreamikcp_write_le16(uint8_t *out, uint16_t value);
void h2_iostreamikcp_write_le32(uint8_t *out, uint32_t value);

#endif
