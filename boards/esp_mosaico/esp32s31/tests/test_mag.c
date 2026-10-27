#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_mosaico_mag.h"
#include <assert.h>
int main(void) {
  assert(!h2_mosaico_mag_valid(INT16_MIN));
  assert(h2_mosaico_mag_valid(0));
  assert(h2_mosaico_mag_valid(32767));
  assert(h2_mosaico_mag_axis(-123) == -1230);
  assert(h2_mosaico_mag_axis(32767) == 327670);
  return 0;
}
