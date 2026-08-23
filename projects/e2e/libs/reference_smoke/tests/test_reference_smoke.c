#include "h2_reference_smoke.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
  assert(h2_reference_smoke_value(UINT32_C(0)) == UINT32_C(0x475A4F54));
  assert(h2_reference_smoke_value(UINT32_C(0x475A4F53)) == UINT32_C(1));
  return 0;
}
