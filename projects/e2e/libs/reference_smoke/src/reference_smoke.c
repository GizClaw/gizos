#include "h2_reference_smoke.h"

uint32_t h2_reference_smoke_value(uint32_t seed) {
  return (seed ^ UINT32_C(0x475A4F53)) + UINT32_C(1);
}
