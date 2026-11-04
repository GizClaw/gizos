#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_mosaico_mag.h"
#include <assert.h>
int main(void) {
  assert(h2_mosaico_mag_axis(0) == 0);
  assert(h2_mosaico_mag_axis(123) == 1230);
  assert(h2_mosaico_mag_axis(-123) == -1230);
  assert(h2_mosaico_mag_axis(INT16_MIN) == -327680);
  assert(h2_mosaico_mag_axis(INT16_MIN + 1) == -327670);
  assert(h2_mosaico_mag_axis(INT16_MAX) == 327670);
  return 0;
}
