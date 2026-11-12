#include "h2_g99_haptics.h"

#include "h2_g99.h"

h2_pal_result_t h2_g99_haptics_pulse(h2_g99_haptics_t* haptics,
                                     h2_runtime_t* runtime, uint64_t now_ms) {
  if (haptics == NULL || runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!haptics->active) {
    const h2_pal_result_t rc = h2_pal_pwm_switch_set_duty(
        runtime->pwm_switch, H2_G99_COMPONENT_HAPTICS, 2000u);
    if (rc != H2_PAL_OK) return rc;
    haptics->active = true;
  }
  haptics->stop_ms = now_ms > UINT64_MAX - 60u ? UINT64_MAX : now_ms + 60u;
  return H2_PAL_OK;
}

h2_pal_result_t h2_g99_haptics_stop(h2_g99_haptics_t* haptics,
                                    h2_runtime_t* runtime) {
  if (haptics == NULL || runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!haptics->active) return H2_PAL_OK;
  const h2_pal_result_t rc = h2_pal_pwm_switch_set_duty(
      runtime->pwm_switch, H2_G99_COMPONENT_HAPTICS, 0u);
  if (rc == H2_PAL_OK) haptics->active = false;
  return rc;
}

h2_pal_result_t h2_g99_haptics_tick(h2_g99_haptics_t* haptics,
                                    h2_runtime_t* runtime, uint64_t now_ms) {
  if (haptics == NULL || runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
  return haptics->active && now_ms >= haptics->stop_ms
             ? h2_g99_haptics_stop(haptics, runtime)
             : H2_PAL_OK;
}
