#include "h2_carrier_setup_failure.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

_Noreturn void h2_carrier_setup_fail(h2_carrier_device_inputs_t* inputs,
                                     const char* stage, int result) {
  h2_carrier_device_inputs_clear(inputs);
  printf("H2_CARRIER_SETUP_FAIL stage=%s rc=%d\n", stage, result);
  fflush(stdout);
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000u));
}
