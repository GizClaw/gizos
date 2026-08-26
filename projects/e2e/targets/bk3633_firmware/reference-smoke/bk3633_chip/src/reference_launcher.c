#include "h2_reference_smoke.h"

#include <stdint.h>

volatile uint32_t h2_bk3633_reference_link_probe;

void h2_bk3633_reference_probe(void) {
  h2_bk3633_reference_link_probe = h2_reference_smoke_value(UINT32_C(3633));
}
