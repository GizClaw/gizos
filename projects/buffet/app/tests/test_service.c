#include "buffet_draw.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Sample the steel alone so a moving plate cannot make a stationary track
 * pass. A quarter-link step is eight backing pixels on each straight run. */
static unsigned difference(const uint16_t* first, const uint16_t* next, int row,
                           int shift) {
  unsigned error = 0;
  for (int x = 560; x < 1040; ++x) {
    error += (unsigned)abs((int)first[row * 1600 + x + shift] -
                           (int)next[row * 1600 + x]);
  }
  return error;
}

int main(void) {
  const size_t count = 1600u * 960u;
  uint16_t* first = malloc(count * sizeof(*first));
  uint16_t* next = malloc(count * sizeof(*next));
  if (!first || !next) return 1;
  const uint16_t ground = 0x8c50;
  for (size_t i = 0; i < count; ++i) first[i] = next[i] = ground;
  bf_steel_track((bf_canvas_t){first, false}, 0);
  bf_steel_track((bf_canvas_t){next, false}, 105);
  int bad =
      difference(first, next, 388, -8) >= difference(first, next, 388, 8) ||
      difference(first, next, 512, 8) >= difference(first, next, 512, -8);
  size_t changed = 0;
  for (size_t i = 0; i < count; ++i) {
    if (first[i] == next[i]) continue;
    int x = (int)(i % 1600), y = (int)(i / 1600);
    if (x < 272 || y < 336 || x >= 1328 || y >= 616) bad = 1;
    ++changed;
  }
  if (changed < 1000) bad = 1;
  for (size_t i = 0; i < count; ++i) next[i] = ground;
  bf_steel_track((bf_canvas_t){next, false}, bf_rules.lap_ms);
  if (memcmp(first, next, count * sizeof(*first)) != 0) bad = 1;
  free(first);
  free(next);
  return bad;
}
