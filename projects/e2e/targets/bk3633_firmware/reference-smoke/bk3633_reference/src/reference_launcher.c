#include "h2_reference_smoke.h"

#include <stdint.h>

volatile uint32_t h2_bk3633_reference_link_probe;

void h2_bk3633_reference_probe(void) {
  h2_bk3633_reference_link_probe = h2_reference_smoke_value(UINT32_C(3633));
}

/* The pinned SDK reference Makefile omits these board hooks from its source list. */
uint8_t check_low_volt_sleep(void) { return UINT8_C(0); }

void adc_init(uint8_t channel, uint8_t mode) {
  (void)channel;
  (void)mode;
}

void adc_isr(void) {}
