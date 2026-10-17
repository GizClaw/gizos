#include "h2_amoled_display_capture.h"
#include "h2_display_esp_capture.h"

const h2_display_esp_capture_t *h2_display_esp_capture_fixture(void) {
  static const h2_display_esp_capture_t fixture = {
      .width = 368, .height = 448, .source = "completed-SPI-DMA",
      .set_capture = h2_amoled_display_set_transfer_capture};
  return &fixture;
}
