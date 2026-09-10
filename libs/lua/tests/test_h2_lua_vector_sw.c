#include "../src/modules/h2_lua_vector_sw.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "h2_lua_vector_delta_cases.h"

static uint8_t data[4096];
static size_t length;
static void byte(unsigned v) {
  assert(length < sizeof(data));
  data[length++] = (uint8_t)v;
}
static void real(float v) {
  uint32_t n;
  memcpy(&n, &v, 4);
  for (int i = 0; i < 4; i++)
    byte(n >> (i * 8));
}
static void point(unsigned op, float x, float y) {
  byte(op);
  real(x);
  real(y);
}
static void begin(void) {
  const uint8_t header[] = {'H', '2', 'V', 'G', 8, 0, 8, 0, 0, 0, 1, 0};
  memcpy(data, header, sizeof(header));
  length = sizeof(header);
}
static void rectangle(float x, float y, float w, float h) {
  byte(4);
  point(5, x, y);
  point(6, x + w, y);
  point(6, x + w, y + h);
  point(6, x, y + h);
  byte(8);
}
static void red(void) {
  byte(10);
  byte(0);
  byte(255);
  byte(255);
  byte(255);
  byte(0);
  byte(0);
  byte(255);
  real(1);
  real(1);
}
int main(void) {
  test_vector_delta(h2_lua_vector_sw_render);
  uint8_t rgba[8 * 8 * 4], again[sizeof(rgba)];
  double identity[] = {1, 0, 0, 1, 0, 0};
  begin();
  rectangle(1, 1, 6, 6);
  red();
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 4) * 4] == 255 && rgba[(4 * 8 + 4) * 4 + 3] == 255);
  assert(rgba[3] == 0);
  assert(h2_lua_vector_sw_render(data, length, again, 8, 8, identity));
  assert(!memcmp(rgba, again, sizeof(rgba)));
  for (size_t n = 0; n < length; n++)
    assert(!h2_lua_vector_sw_render(data, n, rgba, 8, 8, identity));
  begin();
  byte(1);
  rectangle(0, 0, 4, 8);
  byte(11);
  rectangle(0, 0, 8, 8);
  red();
  byte(2);
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 2) * 4 + 3] == 255 && rgba[(4 * 8 + 6) * 4 + 3] == 0);
  begin();
  for (int i = 0; i < 33; i++)
    byte(1);
  byte(0);
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  begin();
  byte(2);
  byte(0);
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  begin();
  byte(4);
  point(5, NAN, 0);
  byte(0);
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  begin();
  byte(99);
  byte(0);
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  begin();
  byte(4);
  byte(13);
  byte(4);
  byte(0);
  const unsigned xy[] = {4, 4, 28, 4, 28, 28, 4, 28};
  for (unsigned i = 0; i < 8; i++) {
    byte(xy[i]);
    byte(0);
  }
  red();
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 4) * 4] == 255 && rgba[3] == 0);
  for (size_t n = 0; n < length; n++)
    assert(!h2_lua_vector_sw_render(data, n, rgba, 8, 8, identity));
  begin();
  byte(4);
  byte(13);
  byte(255);
  byte(255);
  byte(0);
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, NULL));
  double invalid_matrix[] = {NAN, 0, 0, 1, 0, 0};
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, invalid_matrix));
  invalid_matrix[0] = 0;
  assert(!h2_lua_vector_sw_render(data, length, rgba, 8, 8, invalid_matrix));
  /* Geometry, gradient and affine operations used by the approved artwork. */
  begin();
  byte(4);
  byte(9);
  real(1);
  real(1);
  real(6);
  real(6);
  red();
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 4) * 4 + 3] > 240 && rgba[3] == 0);
  begin();
  rectangle(1, 1, 6, 6);
  red();
  byte(0);
  double translate[] = {.5, 0, 0, .5, 4, 0};
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, translate));
  assert(rgba[(2 * 8 + 5) * 4 + 3] > 240 && rgba[(2 * 8 + 2) * 4 + 3] == 0);
  begin();
  data[8] = 1;
  byte(0);
  byte(2);
  real(0);
  real(0);
  real(1);
  real(0);
  real(0);
  byte(0);
  byte(0);
  byte(0);
  byte(255);
  real(1);
  byte(255);
  byte(255);
  byte(255);
  byte(255);
  rectangle(0, 0, 8, 8);
  byte(10);
  byte(0);
  byte(0);
  byte(0);
  for (unsigned k = 0; k < 4; k++)
    byte(255);
  real(1);
  real(1);
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 1) * 4] < rgba[(4 * 8 + 6) * 4] &&
         rgba[(4 * 8 + 4) * 4 + 3] == 255);
  begin();
  byte(4);
  point(5, 1, 1);
  byte(7);
  real(1);
  real(7);
  real(7);
  real(7);
  real(7);
  real(1);
  byte(8);
  red();
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(3 * 8 + 4) * 4 + 3] > 200 && rgba[3] == 0);
  begin();
  byte(4);
  point(5, 1, 4);
  point(6, 7, 4);
  byte(10);
  byte(1);
  byte(255);
  byte(255);
  byte(255);
  byte(0);
  byte(0);
  byte(255);
  real(2);
  real(1);
  byte(0);
  assert(h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity));
  assert(rgba[(4 * 8 + 4) * 4 + 3] > 240 && rgba[3] == 0);
  /* Deterministic malformed-command sweep, also run under ASan/UBSan. */
  begin();
  rectangle(1, 1, 6, 6);
  red();
  byte(0);
  uint32_t seed = 123;
  for (unsigned i = 0; i < 1000; i++) {
    seed = seed * 1664525u + 1013904223u;
    size_t at = seed % length;
    uint8_t saved = data[at];
    data[at] ^= (uint8_t)(1u << ((seed >> 16) % 8));
    (void)h2_lua_vector_sw_render(data, length, rgba, 8, 8, identity);
    data[at] = saved;
  }
  puts("H2VG: visible fill, clip, determinism, truncation, stack bounds and "
       "invalid commands passed");
  return 0;
}
