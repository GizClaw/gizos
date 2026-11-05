#include "h2_chip_board.h"
#include "h2_reference_smoke.h"

#include <stdint.h>

volatile uint32_t h2_esp_chip_reference_result;

void app_main(void) {
  (void)h2_chip_board_name();
  h2_esp_chip_reference_result = h2_reference_smoke_value(UINT32_C(32));
}
