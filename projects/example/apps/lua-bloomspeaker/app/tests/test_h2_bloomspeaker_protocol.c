#include "h2_bloomspeaker_protocol.h"

#include <assert.h>

int main(void) {
  const uint64_t source = UINT64_C(0x0102030405);
  const uint64_t target = UINT64_C(0x0a0b0c0d0e);
  const uint32_t epoch = UINT32_C(0x10203040);
  uint8_t handshake[H2_BLOOMSPEAKER_HANDSHAKE_SIZE];
  h2_bloomspeaker_handshake_make(handshake, source, target, epoch);
  assert(h2_bloomspeaker_handshake_valid(handshake, sizeof(handshake), source,
                                         target, epoch));
  assert(!h2_bloomspeaker_handshake_valid(handshake, sizeof(handshake) - 1u,
                                          source, target, epoch));
  assert(!h2_bloomspeaker_handshake_valid(handshake, sizeof(handshake),
                                          source + 1u, target, epoch));
  assert(!h2_bloomspeaker_handshake_valid(handshake, sizeof(handshake), source,
                                          target + 1u, epoch));
  assert(!h2_bloomspeaker_handshake_valid(handshake, sizeof(handshake), source,
                                          target, epoch + 1u));
  handshake[0] ^= 1u;
  assert(!h2_bloomspeaker_handshake_valid(handshake, sizeof(handshake), source,
                                          target, epoch));
  assert(!h2_bloomspeaker_handshake_valid(NULL, sizeof(handshake), source,
                                          target, epoch));
  return 0;
}
