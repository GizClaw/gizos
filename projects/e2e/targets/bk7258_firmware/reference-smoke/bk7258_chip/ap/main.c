#include "h2_bk7258_chip.h"
#include "h2_reference_smoke.h"

#include "bk_private/bk_init.h"

#include <stdint.h>

volatile uint32_t h2_bk7258_chip_reference_result;

int main(void) {
  (void)h2_bk7258_chip_ap_name();
  h2_bk7258_chip_reference_result = h2_reference_smoke_value(UINT32_C(7258));
  return bk_init();
}
