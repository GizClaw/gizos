#include "h2_bloomspeaker_protocol.h"

#include <string.h>

static const uint8_t s_handshake_magic[] = {'B', 'S', 'P', '1'};

static void write_le(uint8_t *out, uint64_t value, size_t size) {
  for (size_t index = 0u; index < size; ++index) {
    out[index] = (uint8_t)(value >> (index * 8u));
  }
}

static uint64_t read_le(const uint8_t *data, size_t size) {
  uint64_t value = 0u;
  for (size_t index = 0u; index < size; ++index) {
    value |= (uint64_t)data[index] << (index * 8u);
  }
  return value;
}

void h2_bloomspeaker_handshake_make(
    uint8_t out[H2_BLOOMSPEAKER_HANDSHAKE_SIZE], uint64_t source,
    uint64_t target, uint32_t epoch) {
  memcpy(out, s_handshake_magic, sizeof(s_handshake_magic));
  write_le(out + 4u, source, 5u);
  write_le(out + 9u, target, 5u);
  write_le(out + 14u, epoch, 4u);
}

bool h2_bloomspeaker_handshake_valid(const uint8_t *data, size_t size,
                                     uint64_t source, uint64_t target,
                                     uint32_t epoch) {
  return data != NULL && size == H2_BLOOMSPEAKER_HANDSHAKE_SIZE &&
         memcmp(data, s_handshake_magic, sizeof(s_handshake_magic)) == 0 &&
         read_le(data + 4u, 5u) == source &&
         read_le(data + 9u, 5u) == target &&
         read_le(data + 14u, 4u) == epoch;
}
