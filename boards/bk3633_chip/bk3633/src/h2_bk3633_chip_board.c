#include <stdint.h>

const char *h2_bk3633_chip_board_name(void) { return "bk3633_chip"; }

uint8_t check_low_volt_sleep(void) { return UINT8_C(0); }

void adc_init(uint8_t channel, uint8_t mode) {
  (void)channel;
  (void)mode;
}

void adc_isr(void) {}
