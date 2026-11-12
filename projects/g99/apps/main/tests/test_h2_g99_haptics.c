#include "h2_g99_haptics.h"

#include "h2_g99.h"

#include <assert.h>

typedef struct motor {
  uint16_t duty;
  unsigned calls;
  h2_pal_result_t result;
} motor_t;

static h2_pal_result_t set_duty(void* user, h2_pal_periph_id_t component_id,
                                uint16_t duty) {
  motor_t* motor = user;
  assert(component_id == H2_G99_COMPONENT_HAPTICS);
  ++motor->calls;
  if (motor->result == H2_PAL_OK) motor->duty = duty;
  return motor->result;
}

int main(void) {
  motor_t motor = {0};
  const h2_pal_pwm_switch_vtable_t vtable = {.set_duty = set_duty};
  const h2_pal_pwm_switch_api_t api = {&motor, &vtable};
  h2_runtime_t runtime = {.pwm_switch = &api};
  h2_g99_haptics_t haptics = {0};
  assert(h2_g99_haptics_pulse(&haptics, &runtime, 100u) == H2_PAL_OK);
  assert(haptics.active && motor.duty == 2000u);
  assert(h2_g99_haptics_tick(&haptics, &runtime, 159u) == H2_PAL_OK);
  assert(motor.calls == 1u);
  assert(h2_g99_haptics_pulse(&haptics, &runtime, 150u) == H2_PAL_OK);
  assert(motor.calls == 1u);
  assert(h2_g99_haptics_tick(&haptics, &runtime, 209u) == H2_PAL_OK);
  assert(haptics.active);
  motor.result = H2_PAL_ERR_IO;
  assert(h2_g99_haptics_tick(&haptics, &runtime, 210u) == H2_PAL_ERR_IO);
  assert(haptics.active && motor.duty == 2000u);
  motor.result = H2_PAL_OK;
  assert(h2_g99_haptics_tick(&haptics, &runtime, 215u) == H2_PAL_OK);
  assert(!haptics.active && motor.duty == 0u);
  assert(h2_g99_haptics_pulse(&haptics, &runtime, 300u) == H2_PAL_OK);
  assert(h2_g99_haptics_stop(&haptics, &runtime) == H2_PAL_OK);
  assert(!haptics.active && motor.duty == 0u);
  runtime.pwm_switch = NULL;
  assert(h2_g99_haptics_pulse(&haptics, &runtime, 400u) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(!haptics.active);
  assert(h2_g99_haptics_pulse(NULL, &runtime, 400u) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
