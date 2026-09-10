#ifndef H2_LUA_VECTOR_DELTA_H
#define H2_LUA_VECTOR_DELTA_H

#include <stdint.h>

/* H2VG v2 opcode 14: u16 count, u8 unit (1 or 4 quarter-pixels), then
 * interleaved x/y deltas from (0,0), encoded as canonical zigzag varints.
 * Coordinates must remain in the original signed-16-bit quarter-pixel range.
 * Borrowed input; at most three bytes consumed, no allocation or retained
 * state.
 */
static inline int h2_lua_vector_delta_next(const uint8_t **cursor,
                                           const uint8_t *end, unsigned unit,
                                           int32_t *coordinate) {
  if (unit != 1 && unit != 4)
    return 0;
  uint32_t value = 0;
  for (unsigned shift = 0; shift <= 14; shift += 7) {
    if (*cursor == end)
      return 0;
    unsigned byte = *(*cursor)++;
    value |= (uint32_t)(byte & 127u) << shift;
    if (byte < 128) {
      if (value > 131070u || (shift != 0 && byte == 0))
        return 0;
      int32_t delta = (int32_t)(value >> 1);
      if (value & 1u)
        delta = -delta - 1;
      int64_t next = (int64_t)*coordinate + (int64_t)delta * unit;
      if (next < INT16_MIN || next > INT16_MAX)
        return 0;
      *coordinate = (int32_t)next;
      return 1;
    }
  }
  return 0;
}

#endif
