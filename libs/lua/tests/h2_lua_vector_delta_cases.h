#ifndef H2_LUA_VECTOR_DELTA_CASES_H
#define H2_LUA_VECTOR_DELTA_CASES_H

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef int (*vector_test_render_fn)(const uint8_t *, size_t, uint8_t *,
                                     unsigned, unsigned, const double *);

static void test_vector_delta(vector_test_render_fn render) {
  const double matrix[] = {1, 0, 0, 1, 0, 0};
  const uint8_t paint[] = {10, 0, 255, 255, 255, 0,   0,  255, 0,
                           0,  0, 0,   0,   0,   128, 63, 0};
  uint8_t reference[64] = {'H', '2', 'V', 'G', 8, 0,  8, 0,
                           0,   0,   1,   0,   4, 13, 4, 0};
  uint8_t packed[64] = {'H', '2', 'V', 'G', 8, 0, 8,  0, 0, 0,  2,  0, 4,
                        14,  4,   0,   4,   1, 2, 16, 0, 0, 12, 15, 0};
  const int16_t coordinates[] = {-4, 4, 28, 4, 28, 28, -4, 28};
  for (unsigned i = 0; i < 8; i++) {
    reference[16 + i * 2] = (uint8_t)coordinates[i];
    reference[17 + i * 2] = (uint8_t)((uint16_t)coordinates[i] >> 8);
  }
  memcpy(reference + 32, paint, sizeof(paint));
  memcpy(packed + 25, paint, sizeof(paint));
  const size_t length = 25 + sizeof(paint);
  uint8_t before[8 * 8 * 4], after[sizeof(before)];
  assert(render(reference, 32 + sizeof(paint), before, 8, 8, matrix));
  assert(render(packed, length, after, 8, 8, matrix));
  assert(memcmp(before, after, sizeof(before)) == 0);
  for (size_t n = 0; n < length; n++)
    assert(!render(packed, n, after, 8, 8, matrix));
  packed[10] = 1; /* A v1 reader must not accept the v2 opcode. */
  assert(!render(packed, length, after, 8, 8, matrix));
  packed[10] = 2;
  packed[16] = 3; /* Invalid coordinate unit. */
  assert(!render(packed, length, after, 8, 8, matrix));
  packed[16] = 1;
  packed[17] = 128;
  packed[18] = 0; /* Noncanonical varint. */
  assert(!render(packed, length, after, 8, 8, matrix));
  packed[17] = 128;
  packed[18] = 128;
  packed[19] = 128;
  assert(!render(packed, length, after, 8, 8, matrix));
  packed[17] = 128;
  packed[18] = 128;
  packed[19] = 4; /* +32768. */
  assert(!render(packed, length, after, 8, 8, matrix));
}

#endif
